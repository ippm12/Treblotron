/**
 * camera_api.cpp
 *
 * Camera API implementation using OpenCV. Each detected camera gets its
 * own capture thread that continuously grabs frames. Public functions
 * return cached data so the render loop is never blocked by I/O.
 */

#include "vision/vision.hpp"
#include "vision/wire_calibration.hpp"
#include "debug/scoped_timer.hpp"

#include <opencv2/core.hpp>
#include <opencv2/core/utility.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>
#include <opencv2/imgcodecs.hpp>

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
    uint32_t         logicalIndex = 0;    // Index into f_cameras[] — used for warpCameraFrame

    // Front buffers — written by capture thread under frameMutex, read by any
    // thread that calls get*CameraFrame. cv::Mat shallow-copy-on-assign means
    // the capture thread can clone into these while a reader still holds a
    // shared reference to the previous contents (copy-on-write double buffer).
    cv::Mat          latestRaw;           // RGB, 1280x720ish from the sensor
    cv::Mat          latestWarped;        // RGB, 720x720, empty if not calibrated
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
    cv::Mat warped;

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

            {
                VISION_PROFILE_SCOPE(slot->timings, "cvtColor");
                cv::cvtColor(frame, rgb, cv::COLOR_BGR2RGB);
            }

            bool haveWarp = false;
            if(isCameraCalibrated(slot->logicalIndex))
            {
                VISION_PROFILE_SCOPE(slot->timings, "warp");
                haveWarp = warpCameraFrame(slot->logicalIndex, rgb, warped)
                           && !warped.empty();
            }

            // Publish via clone so any reader holding a shallow copy of the
            // previous frame keeps their buffer intact (copy-on-write double buffer).
            {
                std::lock_guard<std::mutex> lock(slot->frameMutex);
                slot->latestRaw = rgb.clone();
                if(haveWarp)
                {
                    slot->latestWarped = warped.clone();
                }
            }
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
            slot->logicalIndex = count;
            slot->name = "Camera " + std::to_string(cameraNum);

            // Pre-convert first frame so there's something to show immediately
            cv::cvtColor(testFrame, slot->latestRaw, cv::COLOR_BGR2RGB);

            slot->running.store(true, std::memory_order_relaxed);
            slot->captureThread = std::thread(captureLoop, slot.get());

            LOG_INFO(VISION_LOG_ID, "Opened {} at device index {} ({}x{})",
                     slot->name, idx, testFrame.cols, testFrame.rows);

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


// Internal: snapshot a Mat out of the slot (shallow copy under lock) and
// memcpy it into an outFrame so the caller gets a standalone byte buffer.
// Shared across the raw and warped accessors.
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


bool getCameraFrame(uint32_t index, CameraFrame& outFrame)
{
    if(index >= f_cameraCount.load(std::memory_order_acquire))
    {
        return false;
    }

    CameraSlot& slot = *f_cameras[index];

    cv::Mat snapshot;
    {
        std::lock_guard<std::mutex> lock(slot.frameMutex);
        snapshot = slot.latestRaw;  // shallow copy
    }
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
    {
        std::lock_guard<std::mutex> lock(slot.frameMutex);
        snapshot = slot.latestWarped;
    }
    return copyFrameOut(snapshot, outFrame);
}


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
            if(f_cameras[i]->latestRaw.empty())
            {
                LOG_WARNING(VISION_LOG_ID, "saveAllCameraFrames: no frame for camera {}", i);
                continue;
            }
            cv::cvtColor(f_cameras[i]->latestRaw, frameCopy, cv::COLOR_RGB2BGR);
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
