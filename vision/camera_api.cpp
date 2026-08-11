/**
 * camera_api.cpp
 *
 * Camera API implementation using OpenCV. Each detected camera gets its
 * own capture thread that continuously grabs frames. Public functions
 * return cached data so the render loop is never blocked by I/O.
 */

#include "vision/vision.hpp"
#include "detect/wire_calibration.hpp"
#include "debug/scoped_timer.hpp"

#include <opencv2/core.hpp>
#include <opencv2/core/utility.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>
#include <opencv2/imgcodecs.hpp>
#ifdef DARTLENS_HAVE_LOCAL_INFERENCE
#include <opencv2/dnn.hpp>
#endif

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <random>
#include <sstream>
#include <iomanip>
#include <thread>
#include <vector>


// ============================================================================
// UUID v4 generator (internal)
// ============================================================================

static std::string generateUuidV4()
{
    static std::mt19937 gen{std::random_device{}()};
    std::uniform_int_distribution<uint32_t> dist(0, 15);
    std::uniform_int_distribution<uint32_t> dist2(8, 11);

    const char* hex = "0123456789abcdef";
    constexpr int uuidLen = 36;
    constexpr const char* pattern = "xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx";

    std::string uuid(uuidLen, ' ');
    for(int i = 0; i < uuidLen; i++)
    {
        if(pattern[i] == '-')       uuid[i] = '-';
        else if(pattern[i] == '4')  uuid[i] = '4';
        else if(pattern[i] == 'y')  uuid[i] = hex[dist2(gen)];
        else                        uuid[i] = hex[dist(gen)];
    }
    return uuid;
}


// ============================================================================
// Internal camera slot
// ============================================================================

struct CameraSlot
{
    cv::VideoCapture capture;
    int              deviceIndex = -1;    // V4L2 device index (e.g. 0, 2, 4)
    // Index into f_cameras[] — used for warpCameraFrame. Atomic because the
    // capture thread reads it every frame and swapCameraSlots() can rewrite
    // it from the render thread without stopping capture.
    std::atomic<uint32_t> logicalIndex{0};

    // Front buffers — written by capture thread under frameMutex, read by any
    // thread that calls get*CameraFrame. cv::Mat shallow-copy-on-assign means
    // the capture thread can clone into these while a reader still holds a
    // shared reference to the previous contents (copy-on-write double buffer).
    cv::Mat          latestRaw;           // RGB, 1280x720ish from the sensor
    cv::Mat          latestWarped;        // RGB, 720x720, empty if not calibrated
#ifdef DARTLENS_PASSTHROUGH_CAPTURE
    // The UVC cameras already deliver MJPEG (see selectBestResolution), so a
    // client that only forwards frames to an inference server has no reason to
    // decode them and re-encode them — that costs CPU at both ends and puts a
    // second generation of JPEG loss into the model's input. In passthrough
    // mode the capture thread stores the sensor's own compressed bytes and
    // nothing is decoded unless a preview screen actually asks for pixels.
    std::vector<uint8_t> latestCompressed;

    // Set once the driver is confirmed to be handing back real JPEG. If the
    // V4L2 backend ignores CAP_PROP_CONVERT_RGB (which happens with some
    // driver/backend combinations) this stays false and the slot silently
    // behaves exactly as it did before.
    std::atomic<bool> passthroughActive{false};
#endif
#ifdef DARTLENS_HAVE_LOCAL_INFERENCE
    // Pre-prepped seg-engine input plane for this camera — 1×(3*360*640) CV_32F.
    // Produced on the capture thread (resize + blobFromImage) so the inference
    // thread doesn't have to. Empty until the first frame is ready.
    cv::Mat          latestSegPlane;
#endif
    /**
     * Bumped on every publish. Lets a consumer tell a genuinely new frame from
     * the one it already sent — the streaming client is credited faster than
     * the cameras produce, so without this it would forward duplicates.
     */
    std::atomic<uint64_t> frameSeq{0};

    std::mutex       frameMutex;

    std::thread      captureThread;
    std::atomic<bool> running{false};
    std::string      name;

    // Per-thread profiling so we can see where capture time is going independently
    // on each core. Logged every ~2s from the capture thread itself.
    FrameTimings     timings;
    double           lastLogSec = 0.0;
};

// Module state — fixed-size array so the init thread can safely add cameras
// while the render thread reads existing ones (atomic count provides synchronization)
static std::unique_ptr<CameraSlot> f_cameras[EXPECTED_CAMERA_COUNT];
static std::atomic<uint32_t>       f_cameraCount{0};

// Background init thread
static std::thread       f_initThread;
static std::atomic<bool> f_initRunning{false};

// Reference count — both HailoVisionSource and CalibrationScreen independently
// call initializeCameraSystem(). The first call actually brings the camera
// threads up; subsequent calls just bump the count. shutdown() only tears the
// system down when the count hits zero. Without this, a second init() would
// overwrite f_initThread while the previous one was still joinable → terminate.
static uint32_t          f_refCount = 0;


#ifdef DARTLENS_PASSTHROUGH_CAPTURE
/**
 * JPEG quality used only when the driver refuses raw MJPEG and we have to
 * re-encode. 75 is a deliberate step down from the old 85: at that point we are
 * already paying for a second compression generation, so the bandwidth is worth
 * more than the last few percent of quality.
 */
static constexpr int PASSTHROUGH_FALLBACK_QUALITY = 75;


/** A JPEG always starts with the SOI marker FF D8. */
static bool looksLikeJpeg(const cv::Mat& m)
{
    if(m.empty() || m.elemSize() != 1) return false;
    const size_t bytes = m.total();
    if(bytes < 4) return false;
    return m.data[0] == 0xFF && m.data[1] == 0xD8;
}
#endif


// ============================================================================
// Resolution selection
// ============================================================================

static constexpr double TARGET_FPS = 30.0;

// Common resolutions to probe, highest first. The detection models are
// trained on 1280x720 so nothing higher is useful, and USB bandwidth on
// the Pi can't sustain three cameras above 720p anyway.
static constexpr struct { int w; int h; } PROBE_RESOLUTIONS[] = {
    {1280,  720},
};
static constexpr int PROBE_COUNT = sizeof(PROBE_RESOLUTIONS) / sizeof(PROBE_RESOLUTIONS[0]);

/// Select the highest resolution the camera supports at TARGET_FPS.
/// Tries each candidate from highest to lowest; uses camera default as fallback.
static void selectBestResolution(cv::VideoCapture& cap)
{
    // UVC cameras on the Pi default to YUYV, which can't sustain our target
    // resolutions over USB — reads come back empty. MJPG is what the cameras
    // actually advertise for HD modes, so force it before touching w/h.
    cap.set(cv::CAP_PROP_FOURCC,
            cv::VideoWriter::fourcc('M', 'J', 'P', 'G'));
    cap.set(cv::CAP_PROP_FPS, TARGET_FPS);
    cap.set(cv::CAP_PROP_BUFFERSIZE, 2);

    for(int i = 0; i < PROBE_COUNT; i++)
    {
        cap.set(cv::CAP_PROP_FRAME_WIDTH,  PROBE_RESOLUTIONS[i].w);
        cap.set(cv::CAP_PROP_FRAME_HEIGHT, PROBE_RESOLUTIONS[i].h);

        int actualW = static_cast<int>(cap.get(cv::CAP_PROP_FRAME_WIDTH));
        int actualH = static_cast<int>(cap.get(cv::CAP_PROP_FRAME_HEIGHT));

        if(actualW == PROBE_RESOLUTIONS[i].w && actualH == PROBE_RESOLUTIONS[i].h)
        {
            LOG_INFO(VISION_LOG_ID, "Selected {}x{} @ {:.0f}fps",
                     actualW, actualH, TARGET_FPS);
            return;
        }
    }

    int fallbackW = static_cast<int>(cap.get(cv::CAP_PROP_FRAME_WIDTH));
    int fallbackH = static_cast<int>(cap.get(cv::CAP_PROP_FRAME_HEIGHT));
    LOG_WARNING(VISION_LOG_ID,
                "No preferred resolution matched — using camera default {}x{}",
                fallbackW, fallbackH);
}


#ifdef DARTLENS_PASSTHROUGH_CAPTURE
/**
 * Try to make read() hand back the sensor's compressed MJPEG instead of a
 * decoded image.
 *
 * Must run *after* the resolution has been negotiated: V4L2 re-negotiates the
 * stream format when width/height change, which quietly undoes the request.
 * That ordering mistake is why the first version of this never engaged.
 *
 * Two routes, because backends disagree about which property owns this:
 * CAP_PROP_CONVERT_RGB=0 is the documented one, and CAP_PROP_FORMAT=-1 is what
 * some V4L2 builds actually honour. Each is confirmed by reading a frame and
 * checking for a JPEG SOI marker rather than trusting the setter's return —
 * these properties routinely report success and do nothing.
 *
 * On failure the capture is restored to decoding and the caller falls back to
 * decode + re-encode, which is exactly what it did before.
 */
static bool tryEnableRawMjpeg(cv::VideoCapture& cap, int deviceIndex)
{
    cv::Mat probe;

    cap.set(cv::CAP_PROP_CONVERT_RGB, 0);
    if(cap.read(probe) && looksLikeJpeg(probe))
    {
        LOG_INFO(VISION_LOG_ID,
                 "Device {}: MJPEG passthrough via CAP_PROP_CONVERT_RGB "
                 "({} KB/frame, no decode or re-encode)", deviceIndex, probe.total() / 1024);
        return true;
    }

    cap.set(cv::CAP_PROP_FORMAT, -1);
    if(cap.read(probe) && looksLikeJpeg(probe))
    {
        LOG_INFO(VISION_LOG_ID,
                 "Device {}: MJPEG passthrough via CAP_PROP_FORMAT "
                 "({} KB/frame, no decode or re-encode)", deviceIndex, probe.total() / 1024);
        return true;
    }

    cap.set(cv::CAP_PROP_FORMAT, CV_8UC3);
    cap.set(cv::CAP_PROP_CONVERT_RGB, 1);
    LOG_WARNING(VISION_LOG_ID,
                "Device {}: driver will not hand over raw MJPEG — falling back to "
                "decode + re-encode. Streaming still works, it just costs Pi CPU "
                "and a second generation of JPEG loss.", deviceIndex);
    return false;
}
#endif


// ============================================================================
// Capture thread
// ============================================================================

static double nowSeconds()
{
    return std::chrono::duration<double>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

static void captureLoop(CameraSlot* slot)
{
    cv::Mat frame;
    cv::Mat rgb;
#ifndef DARTLENS_HAVE_LOCAL_INFERENCE
    cv::Mat warped;
#endif
#ifdef DARTLENS_HAVE_LOCAL_INFERENCE
    // Reused scratch for the seg-input pipeline. resizedSeg is 360x640 RGB8;
    // segPlane is the 1×(3*360*640) CV_32F NCHW blob blobFromImage writes.
    cv::Mat resizedSeg;
    cv::Mat segPlane;
#endif

    while(slot->running.load(std::memory_order_relaxed))
    {
        {
            VISION_PROFILE_SCOPE(slot->timings, "total");

            bool ok;
            {
                VISION_PROFILE_SCOPE(slot->timings, "read");
                ok = slot->capture.read(frame) && !frame.empty();
            }
            if(!ok) continue;

#ifdef DARTLENS_PASSTHROUGH_CAPTURE
            // With CAP_PROP_CONVERT_RGB=0 the V4L2 backend hands back the
            // sensor's compressed buffer as a single-row CV_8UC1 Mat. Confirm
            // it really is JPEG (SOI marker) rather than trusting the property
            // took effect — some backends quietly ignore it and keep returning
            // decoded BGR, and misreading that as JPEG would ship garbage.
            if(looksLikeJpeg(frame))
            {
                {
                    std::lock_guard<std::mutex> lock(slot->frameMutex);
                    slot->latestCompressed.assign(frame.data,
                                                  frame.data + frame.total() * frame.elemSize());
                }
                slot->frameSeq.fetch_add(1, std::memory_order_release);
                if(!slot->passthroughActive.exchange(true, std::memory_order_acq_rel))
                {
                    LOG_INFO(VISION_LOG_ID,
                             "Camera {} passthrough active — forwarding the sensor's own "
                             "JPEG, no decode/re-encode", slot->deviceIndex);
                }
                continue;   // nothing else on this thread has anything to do
            }

            // Not JPEG: the driver is decoding for us. tryEnableRawMjpeg
            // already reported that at open time, so say nothing here — a
            // per-frame warning on three capture threads would be noise, and
            // the `static bool warned` that used to guard it was shared across
            // all three anyway.
#endif

            {
                VISION_PROFILE_SCOPE(slot->timings, "cvtColor");
                cv::cvtColor(frame, rgb, cv::COLOR_BGR2RGB);
            }

#ifndef DARTLENS_HAVE_LOCAL_INFERENCE
            // Capture-thread warp — only used by the Hailo + sim vision sources
            // and the calibration debug overlay. The TensorRT pipeline does its
            // own warp on the masked frame after segmentation, so paying the
            // ~5-15 ms of warpPerspective on this thread would be pure waste
            // there.
            bool haveWarp = false;
            // Snapshot the logical index once per frame — swapCameraSlots() may
            // rewrite it between the calibrated check and the warp call, and we
            // want both to refer to the same slot.
            uint32_t logical = slot->logicalIndex.load(std::memory_order_acquire);
            if(isCameraCalibrated(logical))
            {
                VISION_PROFILE_SCOPE(slot->timings, "warp");
                haveWarp = warpCameraFrame(logical, rgb, warped)
                           && !warped.empty();
            }
#endif

#ifdef DARTLENS_HAVE_LOCAL_INFERENCE
            // Pre-prep this camera's seg-engine input plane while the other
            // capture threads do the same for theirs. Cheap-by-comparison
            // resize, then blobFromImage produces tightly-packed NCHW float32
            // (1, 3, 360, 640) with SIMD-vectorized internals — vastly faster
            // than the per-pixel scalar loop the inference thread used to run
            // for all 3 cameras serially.
            {
                VISION_PROFILE_SCOPE(slot->timings, "segPlane");
                cv::resize(rgb, resizedSeg, cv::Size(640, 360), 0, 0, cv::INTER_LINEAR);
                cv::dnn::blobFromImage(resizedSeg, segPlane,
                                       /*scalefactor*/ 1.0 / 255.0,
                                       /*size*/        cv::Size(),
                                       /*mean*/        cv::Scalar(),
                                       /*swapRB*/      false,
                                       /*crop*/        false,
                                       CV_32F);
            }
#endif

            // Publish via clone so any reader holding a shallow copy of the
            // previous frame keeps their buffer intact (copy-on-write double buffer).
            {
                std::lock_guard<std::mutex> lock(slot->frameMutex);
                slot->latestRaw = rgb.clone();
#ifndef DARTLENS_HAVE_LOCAL_INFERENCE
                if(haveWarp)
                {
                    slot->latestWarped = warped.clone();
                }
#endif
#ifdef DARTLENS_HAVE_LOCAL_INFERENCE
                // segPlane is exclusive-owned by this thread; clone before
                // publishing so the next iteration's blobFromImage doesn't
                // race a reader that's still mid-memcpy.
                slot->latestSegPlane = segPlane.clone();
#endif
            }
            slot->frameSeq.fetch_add(1, std::memory_order_release);
        }

        slot->timings.nextFrame();

        double t = nowSeconds();
        if(slot->lastLogSec == 0.0) slot->lastLogSec = t;
        if(t - slot->lastLogSec >= 2.0)
        {
            slot->lastLogSec = t;
            std::string line;
            for(const auto& e : slot->timings.snapshot())
            {
                char buf[64];
                std::snprintf(buf, sizeof(buf), "%s=%.1f ", e.name, e.avgMs);
                line += buf;
            }
            LOG_INFO(VISION_LOG_ID, "{} timings: {}", slot->name, line);
        }
    }
}


// ============================================================================
// NEON detection — warn loudly on every run if OpenCV wasn't built with NEON,
// since warpPerspective scales ~20x between the scalar and NEON paths on the
// Pi 5 and is the dominant cost in each capture thread.
// ============================================================================

static void logOpenCVNeonStatus()
{
    if(cv::checkHardwareSupport(CV_CPU_NEON))
    {
        LOG_INFO(VISION_LOG_ID, "OpenCV runtime NEON support: YES");
    }
    else
    {
        LOG_WARNING(VISION_LOG_ID,
                    "OpenCV runtime NEON support: NO — camera system may run slowly; "
                    "rebuild OpenCV with NEON enabled for a major speedup");
    }

    // Runtime CPU feature detection can report NEON even when the OpenCV build
    // itself wasn't compiled with NEON intrinsics in the hot paths (warpPerspective
    // in particular). Dump the build info once so we can see the compile-time
    // CPU_BASELINE / CPU_DISPATCH values and confirm warp is actually vectorized.
    const cv::String info = cv::getBuildInformation();
    std::stringstream ss(info);
    std::string line;
    while(std::getline(ss, line))
    {
        LOG_INFO(VISION_LOG_ID, "cv::getBuildInformation | {}", line);
    }
}


// ============================================================================
// Public API
// ============================================================================

Status initializeCameraSystem()
{
    if(f_refCount++ > 0)
    {
        return STATUS_OK;
    }

    logOpenCVNeonStatus();

    initializeWireCalibration();
    // Load any saved wire calibration synchronously before the user can interact
    // with the calibration screen, so there's no race with the init thread.
    loadWireCalibration();

    f_initRunning.store(true, std::memory_order_relaxed);

    f_initThread = std::thread([]()
    {
        static constexpr int MAX_PROBE = 8;

        for(int idx = 0; idx < MAX_PROBE; idx++)
        {
            if(!f_initRunning.load(std::memory_order_relaxed))
            {
                break;  // Shutdown requested before probing finished
            }

            // Force the V4L2 backend. OpenCV's default auto-selection prefers
            // GStreamer, which builds a v4l2src pipeline that silently ignores
            // CAP_PROP_FOURCC — our MJPG request never reaches the driver and
            // the pipeline fails to negotiate a format. V4L2 honors fourcc.
            cv::VideoCapture cap;
            if(!cap.open(idx, cv::CAP_V4L2))
            {
                continue;
            }

            selectBestResolution(cap);
#ifdef DARTLENS_PASSTHROUGH_CAPTURE
            tryEnableRawMjpeg(cap, idx);
#endif

            // Verify we can actually grab a frame
            cv::Mat testFrame;
            if(!cap.read(testFrame) || testFrame.empty())
            {
                cap.release();
                continue;
            }

            uint32_t count = f_cameraCount.load(std::memory_order_relaxed);
            uint32_t cameraNum = count + 1;

            auto slot = std::make_unique<CameraSlot>();
            slot->capture = std::move(cap);
            slot->deviceIndex = idx;
            slot->logicalIndex.store(count, std::memory_order_relaxed);
            slot->name = "Camera " + std::to_string(cameraNum);

            // Pre-convert first frame so there's something to show immediately.
            // In passthrough mode read() hands back the sensor's compressed
            // buffer as a single-row CV_8UC1, which cvtColor would reject
            // outright — decode it instead, and record the real frame size so
            // the log below reports 1280x720 rather than a JPEG byte count.
            int probeW = testFrame.cols;
            int probeH = testFrame.rows;
#ifdef DARTLENS_PASSTHROUGH_CAPTURE
            if(looksLikeJpeg(testFrame))
            {
                const cv::Mat decoded = cv::imdecode(testFrame, cv::IMREAD_COLOR_RGB);
                if(decoded.empty())
                {
                    LOG_WARNING(VISION_LOG_ID,
                                "Device {} returned undecodable MJPEG — skipping", idx);
                    slot->capture.release();
                    continue;
                }
                slot->latestRaw = decoded;
                probeW = decoded.cols;
                probeH = decoded.rows;
            }
            else
#endif
            {
                cv::cvtColor(testFrame, slot->latestRaw, cv::COLOR_BGR2RGB);
            }

            slot->running.store(true, std::memory_order_relaxed);
            slot->captureThread = std::thread(captureLoop, slot.get());

            LOG_INFO(VISION_LOG_ID, "Opened {} at device index {} ({}x{})",
                     slot->name, idx, probeW, probeH);

            // Store slot then publish the new count — acquire/release ordering
            // ensures the render thread sees the fully constructed slot
            f_cameras[count] = std::move(slot);
            f_cameraCount.store(cameraNum, std::memory_order_release);

            if(cameraNum >= EXPECTED_CAMERA_COUNT)
            {
                break;
            }
        }

        LOG_INFO(VISION_LOG_ID, "Camera system initialized: {} camera(s) detected",
                 f_cameraCount.load(std::memory_order_relaxed));

        // Auto-load saved wire calibration if present
        loadWireCalibration();

        f_initRunning.store(false, std::memory_order_relaxed);
    });

    return STATUS_OK;
}


void shutdownCameraSystem()
{
    if(f_refCount == 0 || --f_refCount > 0)
    {
        return;
    }

    // Stop the init thread if still probing cameras
    f_initRunning.store(false, std::memory_order_relaxed);
    if(f_initThread.joinable())
    {
        f_initThread.join();
    }

    uint32_t count = f_cameraCount.load(std::memory_order_acquire);

    for(uint32_t i = 0; i < count; i++)
    {
        f_cameras[i]->running.store(false, std::memory_order_relaxed);
    }

    for(uint32_t i = 0; i < count; i++)
    {
        if(f_cameras[i]->captureThread.joinable())
        {
            f_cameras[i]->captureThread.join();
        }
        f_cameras[i]->capture.release();
        f_cameras[i].reset();
    }

    f_cameraCount.store(0, std::memory_order_relaxed);

    shutdownWireCalibration();

    LOG_INFO(VISION_LOG_ID, "Camera system shut down");
}


uint32_t getCameraCount()
{
    return f_cameraCount.load(std::memory_order_acquire);
}


std::string getCameraName(uint32_t index)
{
    if(index >= f_cameraCount.load(std::memory_order_acquire))
    {
        return "Camera " + std::to_string(index + 1);
    }
    return f_cameras[index]->name;
}


bool swapCameraSlots(uint32_t a, uint32_t b)
{
    uint32_t count = f_cameraCount.load(std::memory_order_acquire);
    if(a == b || a >= count || b >= count) return false;

    // Move the slot pointers — calibration in wire_calibration.cpp is keyed by
    // logical slot index and stays put, so the camera now sitting at slot `a`
    // picks up slot `a`'s calibration. logicalIndex is what the capture thread
    // reads to decide which calibration to apply, so update both atomically.
    std::swap(f_cameras[a], f_cameras[b]);
    f_cameras[a]->logicalIndex.store(a, std::memory_order_release);
    f_cameras[b]->logicalIndex.store(b, std::memory_order_release);

    // Keep the displayed name aligned with the logical slot so "Camera N"
    // always means the slot using calibration N.
    f_cameras[a]->name = "Camera " + std::to_string(a + 1);
    f_cameras[b]->name = "Camera " + std::to_string(b + 1);

    LOG_INFO(VISION_LOG_ID, "Swapped camera slots {} and {}", a, b);
    return true;
}


// Internal: snapshot a Mat out of the slot (shallow copy under lock) and
// memcpy it into an outFrame so the caller gets a standalone byte buffer.
// Shared across the raw and warped accessors.
#ifdef DARTLENS_PASSTHROUGH_CAPTURE
/**
 * Latest frame as decoded RGB, whichever mode the slot is in.
 *
 * In passthrough mode the decode happens here, on the caller's thread, rather
 * than on the capture thread — so it costs nothing during normal play and only
 * shows up when a screen (calibration, vision_debug) actually wants pixels.
 */
static bool decodeLatestRgb(CameraSlot& slot, cv::Mat& out)
{
    if(slot.passthroughActive.load(std::memory_order_acquire))
    {
        std::vector<uint8_t> jpeg;
        {
            std::lock_guard<std::mutex> lock(slot.frameMutex);
            jpeg = slot.latestCompressed;
        }
        if(jpeg.empty()) return false;

        const cv::Mat bgr = cv::imdecode(jpeg, cv::IMREAD_COLOR);
        if(bgr.empty()) return false;
        cv::cvtColor(bgr, out, cv::COLOR_BGR2RGB);
        return true;
    }

    std::lock_guard<std::mutex> lock(slot.frameMutex);
    out = slot.latestRaw;   // shallow copy; copy-on-write keeps it valid
    return !out.empty();
}
#endif


static bool copyFrameOut(const cv::Mat& src, CameraFrame& outFrame)
{
    if(src.empty()) return false;

    outFrame.width  = src.cols;
    outFrame.height = src.rows;
    outFrame.stride = static_cast<int>(src.step[0]);

    size_t dataSize = static_cast<size_t>(outFrame.height) * outFrame.stride;
    outFrame.pixels.resize(dataSize);
    std::memcpy(outFrame.pixels.data(), src.data, dataSize);
    return true;
}


#ifdef DARTLENS_PASSTHROUGH_CAPTURE
bool getCameraCompressedFrame(uint32_t index, std::vector<uint8_t>& out)
{
    if(index >= f_cameraCount.load(std::memory_order_acquire)) return false;

    CameraSlot& slot = *f_cameras[index];

    if(slot.passthroughActive.load(std::memory_order_acquire))
    {
        std::lock_guard<std::mutex> lock(slot.frameMutex);
        if(slot.latestCompressed.empty()) return false;
        out = slot.latestCompressed;   // already JPEG — nothing to do
        return true;
    }

    // Fallback for a driver that wouldn't hand over raw MJPEG: encode from the
    // decoded frame, which is exactly what the client used to do unconditionally.
    cv::Mat rgb;
    {
        std::lock_guard<std::mutex> lock(slot.frameMutex);
        rgb = slot.latestRaw;
    }
    if(rgb.empty()) return false;

    cv::Mat bgr;
    cv::cvtColor(rgb, bgr, cv::COLOR_RGB2BGR);
    return cv::imencode(".jpg", bgr, out,
                        {cv::IMWRITE_JPEG_QUALITY, PASSTHROUGH_FALLBACK_QUALITY});
}
#endif


uint64_t getCameraFrameSequence(uint32_t index)
{
    if(index >= f_cameraCount.load(std::memory_order_acquire)) return 0;
    return f_cameras[index]->frameSeq.load(std::memory_order_acquire);
}


bool getCameraFrame(uint32_t index, CameraFrame& outFrame)
{
    if(index >= f_cameraCount.load(std::memory_order_acquire))
    {
        return false;
    }

    CameraSlot& slot = *f_cameras[index];

    cv::Mat snapshot;
#ifdef DARTLENS_PASSTHROUGH_CAPTURE
    if(!decodeLatestRgb(slot, snapshot)) return false;
#else
    {
        std::lock_guard<std::mutex> lock(slot.frameMutex);
        snapshot = slot.latestRaw;  // shallow copy
    }
#endif
    return copyFrameOut(snapshot, outFrame);
}


bool getCameraWarpedFrame(uint32_t index, CameraFrame& outFrame)
{
    if(index >= f_cameraCount.load(std::memory_order_acquire))
    {
        return false;
    }

    CameraSlot& slot = *f_cameras[index];

    cv::Mat snapshot;
#ifdef DARTLENS_PASSTHROUGH_CAPTURE
    if(slot.passthroughActive.load(std::memory_order_acquire))
    {
        // The capture thread never decoded, so there is no cached warp. Only
        // the calibration and vision_debug screens ask for this, so paying for
        // it on their thread is the right trade.
        cv::Mat rgb;
        if(!decodeLatestRgb(slot, rgb)) return false;
        cv::Mat warped;
        if(!warpCameraFrame(slot.logicalIndex.load(std::memory_order_acquire), rgb, warped)
        || warped.empty())
        {
            return false;
        }
        return copyFrameOut(warped, outFrame);
    }
#endif
    {
        std::lock_guard<std::mutex> lock(slot.frameMutex);
        snapshot = slot.latestWarped;
    }
    return copyFrameOut(snapshot, outFrame);
}


#ifdef DARTLENS_HAVE_LOCAL_INFERENCE
bool getCameraSegPlane(uint32_t index, float* out, size_t floatCount)
{
    if(!out) return false;
    if(index >= f_cameraCount.load(std::memory_order_acquire)) return false;

    CameraSlot& slot = *f_cameras[index];

    // Shallow-copy out under the lock; the actual byte memcpy is done outside
    // so we don't hold the capture thread off for the full 2.7 MB transfer.
    cv::Mat snapshot;
    {
        std::lock_guard<std::mutex> lock(slot.frameMutex);
        snapshot = slot.latestSegPlane;
    }
    if(snapshot.empty() || !snapshot.isContinuous()) return false;
    const size_t n = static_cast<size_t>(snapshot.total());
    if(n > floatCount) return false;

    std::memcpy(out, snapshot.ptr<float>(), n * sizeof(float));
    return true;
}


void publishCameraWarpedFrame(uint32_t index,
                              const uint8_t* pixels,
                              int width, int height, int stride)
{
    if(!pixels || width <= 0 || height <= 0 || stride <= 0) return;
    if(index >= f_cameraCount.load(std::memory_order_acquire)) return;

    CameraSlot& slot = *f_cameras[index];

    // Build a Mat that owns its pixel buffer (clone of an external view) so
    // readers don't race a caller that reuses its source memory next frame.
    cv::Mat view(height, width, CV_8UC3,
                 const_cast<uint8_t*>(pixels), static_cast<size_t>(stride));
    cv::Mat owned = view.clone();

    std::lock_guard<std::mutex> lock(slot.frameMutex);
    slot.latestWarped = std::move(owned);
}
#endif


Status saveAllCameraFrames(const std::string& outputDir)
{
    uint32_t count = f_cameraCount.load(std::memory_order_acquire);
    if(count == 0)
    {
        LOG_WARNING(VISION_LOG_ID, "saveAllCameraFrames: no cameras available");
        return STATUS_ERROR_GENERIC;
    }

    std::filesystem::create_directories(outputDir);

    // One UUID per capture — all cameras in this set share it
    std::string uuid = generateUuidV4();
    uint32_t saved = 0;

    for(uint32_t i = 0; i < count; i++)
    {
        cv::Mat frameCopy;
        {
            std::lock_guard<std::mutex> lock(f_cameras[i]->frameMutex);
#ifdef DARTLENS_PASSTHROUGH_CAPTURE
            cv::Mat decoded;
            if(!decodeLatestRgb(*f_cameras[i], decoded)) continue;
            cv::cvtColor(decoded, frameCopy, cv::COLOR_RGB2BGR);
#else
            if(f_cameras[i]->latestRaw.empty())
            {
                LOG_WARNING(VISION_LOG_ID, "saveAllCameraFrames: no frame for camera {}", i);
                continue;
            }
            cv::cvtColor(f_cameras[i]->latestRaw, frameCopy, cv::COLOR_RGB2BGR);
#endif
        }

        std::string path = outputDir + "/" + uuid + "_cam" + std::to_string(i) + ".png";

        if(!cv::imwrite(path, frameCopy))
        {
            LOG_ERROR(VISION_LOG_ID, "Failed to write frame to {}", path);
            continue;
        }

        LOG_INFO(VISION_LOG_ID, "Saved camera {} frame to {}", i, path);
        saved++;
    }

    return (saved > 0) ? STATUS_OK : STATUS_ERROR_GENERIC;
}
