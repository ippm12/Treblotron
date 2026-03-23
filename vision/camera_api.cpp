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

// Module state — vector of unique_ptrs so CameraSlot (non-movable due to mutex) works
static std::vector<std::unique_ptr<CameraSlot>> f_cameras;


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
    static constexpr int MAX_PROBE = 8;

    for(int idx = 0; idx < MAX_PROBE; idx++)
    {
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

        // Sequential 1-based name for display; store device index for logging
        uint32_t cameraNum = static_cast<uint32_t>(f_cameras.size()) + 1;

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

        f_cameras.push_back(std::move(slot));

        if(f_cameras.size() >= EXPECTED_CAMERA_COUNT)
        {
            break;
        }
    }

    LOG_INFO(VISION_LOG_ID, "Camera system initialized: {} camera(s) detected",
             f_cameras.size());
    return STATUS_OK;
}


void shutdownCameraSystem()
{
    for(auto& slot : f_cameras)
    {
        slot->running.store(false, std::memory_order_relaxed);
    }

    for(auto& slot : f_cameras)
    {
        if(slot->captureThread.joinable())
        {
            slot->captureThread.join();
        }
        slot->capture.release();

        if(slot->sdlSurface)
        {
            SDL_DestroySurface(slot->sdlSurface);
            slot->sdlSurface = nullptr;
        }
    }

    f_cameras.clear();
    LOG_INFO(VISION_LOG_ID, "Camera system shut down");
}


uint32_t getCameraCount()
{
    return static_cast<uint32_t>(f_cameras.size());
}


std::string getCameraName(uint32_t index)
{
    if(index >= f_cameras.size())
    {
        return "Camera " + std::to_string(index + 1);
    }
    return f_cameras[index]->name;
}


SDL_Surface* getCameraFrame(uint32_t index)
{
    if(index >= f_cameras.size())
    {
        return nullptr;
    }

    CameraSlot& slot = *f_cameras[index];

    // Only update when the capture thread has a new frame.
    // Use try_lock so we never block the render loop.
    if(slot.frameNew.load(std::memory_order_acquire))
    {
        std::unique_lock<std::mutex> lock(slot.frameMutex, std::try_to_lock);
        if(lock.owns_lock())
        {
            // Free previous surface before replacing backing memory
            if(slot.sdlSurface)
            {
                SDL_DestroySurface(slot.sdlSurface);
                slot.sdlSurface = nullptr;
            }

            // Take ownership of the RGB frame (already converted on capture thread)
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
        // If lock failed, we just return the previous surface — no stall
    }

    return slot.sdlSurface;
}


Status saveCameraFrame(uint32_t cameraIndex, const std::string& outputDir)
{
    if(cameraIndex >= f_cameras.size())
    {
        LOG_WARNING(VISION_LOG_ID, "saveCameraFrame: camera index {} out of range", cameraIndex);
        return STATUS_ERROR_INVALID_PARAM;
    }

    cv::Mat frameCopy;
    {
        std::lock_guard<std::mutex> lock(f_cameras[cameraIndex]->frameMutex);
        if(f_cameras[cameraIndex]->latestFrame.empty())
        {
            // Fall back to the surface backing if capture thread hasn't written yet
            if(f_cameras[cameraIndex]->surfaceBacking.empty())
            {
                LOG_WARNING(VISION_LOG_ID, "saveCameraFrame: no frame available for camera {}", cameraIndex);
                return STATUS_ERROR_GENERIC;
            }
            // surfaceBacking is RGB; convert back to BGR for imwrite
            cv::cvtColor(f_cameras[cameraIndex]->surfaceBacking, frameCopy, cv::COLOR_RGB2BGR);
        }
        else
        {
            // latestFrame is RGB; convert back to BGR for imwrite
            cv::cvtColor(f_cameras[cameraIndex]->latestFrame, frameCopy, cv::COLOR_RGB2BGR);
        }
    }

    std::filesystem::create_directories(outputDir);

    std::string uuid = generateUuidV4();
    std::string path = outputDir + "/cam" + std::to_string(cameraIndex) + "_" + uuid + ".png";

    if(!cv::imwrite(path, frameCopy))
    {
        LOG_ERROR(VISION_LOG_ID, "Failed to write frame to {}", path);
        return STATUS_ERROR_GENERIC;
    }

    LOG_INFO(VISION_LOG_ID, "Saved camera {} frame to {}", cameraIndex, path);
    return STATUS_OK;
}
