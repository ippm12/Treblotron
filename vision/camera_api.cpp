/**
 * camera_api.cpp
 *
 * Camera API implementation using OpenCV. Each detected camera gets its
 * own capture thread that continuously grabs frames. Public functions
 * return cached data so the render loop is never blocked by I/O.
 */

#include "vision/vision.hpp"

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>
#include <opencv2/imgcodecs.hpp>

#include <SDL3/SDL_surface.h>

#include <atomic>
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
    cv::Mat          latestFrame;       // RGB, ready for SDL
    std::mutex       frameMutex;
    std::atomic<bool> frameNew{false};  // True when capture thread wrote a new frame

    // Owned by render thread — only touched by getCameraFrame()
    SDL_Surface*     sdlSurface = nullptr;
    cv::Mat          surfaceBacking;    // Keeps memory alive for sdlSurface

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
        f_initRunning.store(false, std::memory_order_relaxed);
    });

    return STATUS_OK;
}


void shutdownCameraSystem()
{
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

        if(f_cameras[i]->sdlSurface)
        {
            SDL_DestroySurface(f_cameras[i]->sdlSurface);
            f_cameras[i]->sdlSurface = nullptr;
        }

        f_cameras[i].reset();
    }

    f_cameraCount.store(0, std::memory_order_relaxed);
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


SDL_Surface* getCameraFrame(uint32_t index)
{
    if(index >= f_cameraCount.load(std::memory_order_acquire))
    {
        return nullptr;
    }

    CameraSlot& slot = *f_cameras[index];

    // Check the flag first — no lock needed if nothing changed
    if(slot.frameNew.load(std::memory_order_acquire))
    {
        // Blocking lock is fine here — the capture thread only holds it
        // for the duration of a clone(), which is fast
        std::lock_guard<std::mutex> lock(slot.frameMutex);

        if(slot.sdlSurface)
        {
            SDL_DestroySurface(slot.sdlSurface);
            slot.sdlSurface = nullptr;
        }

        slot.surfaceBacking = std::move(slot.latestFrame);
        slot.frameNew.store(false, std::memory_order_release);

        slot.sdlSurface = SDL_CreateSurfaceFrom(
            slot.surfaceBacking.cols,
            slot.surfaceBacking.rows,
            SDL_PIXELFORMAT_RGB24,
            slot.surfaceBacking.data,
            static_cast<int>(slot.surfaceBacking.step[0])
        );
    }

    return slot.sdlSurface;
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
                if(f_cameras[i]->surfaceBacking.empty())
                {
                    LOG_WARNING(VISION_LOG_ID, "saveAllCameraFrames: no frame for camera {}", i);
                    continue;
                }
                cv::cvtColor(f_cameras[i]->surfaceBacking, frameCopy, cv::COLOR_RGB2BGR);
            }
            else
            {
                cv::cvtColor(f_cameras[i]->latestFrame, frameCopy, cv::COLOR_RGB2BGR);
            }
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
