/**
 * camera_api.cpp
 *
 * Camera API implementation using OpenCV. Each detected camera gets its
 * own capture thread that continuously grabs frames. Public functions
 * return cached data so the render loop is never blocked by I/O.
 */

#include "vision/vision.hpp"
#include "vision/wire_calibration.hpp"

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>
#include <opencv2/imgcodecs.hpp>

#include <atomic>
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
    int              deviceIndex = -1;  // V4L2 device index (e.g. 0, 2, 4)

    // Written by capture thread under lock
    cv::Mat          latestFrame;       // RGB
    std::mutex       frameMutex;
    std::atomic<bool> frameNew{false};  // True when capture thread wrote a new frame

    std::thread      captureThread;
    std::atomic<bool> running{false};
    std::string      name;
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

// Common resolutions to probe, highest first
static constexpr struct { int w; int h; } PROBE_RESOLUTIONS[] = {
    {1920, 1080},
    {1600,  900},
    {1280,  720},
    {1024,  768},
    { 800,  600},
    { 640,  480},
};
static constexpr int PROBE_COUNT = sizeof(PROBE_RESOLUTIONS) / sizeof(PROBE_RESOLUTIONS[0]);

/// Select the highest resolution the camera supports at TARGET_FPS.
/// Tries each candidate from highest to lowest; uses camera default as fallback.
static void selectBestResolution(cv::VideoCapture& cap)
{
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

static void captureLoop(CameraSlot* slot)
{
    cv::Mat frame;
    cv::Mat rgb;
    while(slot->running.load(std::memory_order_relaxed))
    {
        if(slot->capture.read(frame) && !frame.empty())
        {
            // Convert BGR → RGB on the capture thread (not the render thread)
            cv::cvtColor(frame, rgb, cv::COLOR_BGR2RGB);

            std::lock_guard<std::mutex> lock(slot->frameMutex);
            slot->latestFrame = rgb.clone();
            slot->frameNew.store(true, std::memory_order_release);
        }
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

            cv::VideoCapture cap;
            if(!cap.open(idx))
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
            slot->name = "Camera " + std::to_string(cameraNum);

            // Pre-convert first frame so there's something to show immediately
            cv::cvtColor(testFrame, slot->latestFrame, cv::COLOR_BGR2RGB);
            slot->frameNew.store(true, std::memory_order_release);

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


bool getCameraFrame(uint32_t index, CameraFrame& outFrame)
{
    if(index >= f_cameraCount.load(std::memory_order_acquire))
    {
        return false;
    }

    CameraSlot& slot = *f_cameras[index];

    if(!slot.frameNew.load(std::memory_order_acquire))
    {
        return false;
    }

    std::lock_guard<std::mutex> lock(slot.frameMutex);

    if(slot.latestFrame.empty())
    {
        return false;
    }

    outFrame.width  = slot.latestFrame.cols;
    outFrame.height = slot.latestFrame.rows;
    outFrame.stride = static_cast<int>(slot.latestFrame.step[0]);

    size_t dataSize = static_cast<size_t>(outFrame.height) * outFrame.stride;
    outFrame.pixels.resize(dataSize);
    std::memcpy(outFrame.pixels.data(), slot.latestFrame.data, dataSize);

    slot.frameNew.store(false, std::memory_order_release);
    return true;
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
            if(f_cameras[i]->latestFrame.empty())
            {
                LOG_WARNING(VISION_LOG_ID, "saveAllCameraFrames: no frame for camera {}", i);
                continue;
            }
            cv::cvtColor(f_cameras[i]->latestFrame, frameCopy, cv::COLOR_RGB2BGR);
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
