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
    cv::Mat          latestFrame;   // BGR, written by capture thread
    cv::Mat          rgbFrame;      // RGB, backing memory for sdlSurface
    SDL_Surface*     sdlSurface = nullptr;
    std::thread      captureThread;
    std::mutex       frameMutex;
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
    while(slot->running.load(std::memory_order_relaxed))
    {
        if(slot->capture.read(frame) && !frame.empty())
        {
            std::lock_guard<std::mutex> lock(slot->frameMutex);
            slot->latestFrame = frame.clone();
        }
    }
}


// ============================================================================
// Public API
// ============================================================================

Status initializeCameraSystem()
{
    // Probe camera indices to find connected cameras
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

        auto slot = std::make_unique<CameraSlot>();
        slot->capture = std::move(cap);
        slot->latestFrame = testFrame.clone();
        slot->name = "Camera " + std::to_string(idx);
        slot->running.store(true, std::memory_order_relaxed);
        slot->captureThread = std::thread(captureLoop, slot.get());

        LOG_INFO(VISION_LOG_ID, "Opened camera at index {} ({}x{})",
                 idx, testFrame.cols, testFrame.rows);

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
        return "Camera " + std::to_string(index);
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
    std::lock_guard<std::mutex> lock(slot.frameMutex);

    if(slot.latestFrame.empty())
    {
        return nullptr;
    }

    // Convert BGR → RGB for SDL
    cv::cvtColor(slot.latestFrame, slot.rgbFrame, cv::COLOR_BGR2RGB);

    // Free previous surface
    if(slot.sdlSurface)
    {
        SDL_DestroySurface(slot.sdlSurface);
        slot.sdlSurface = nullptr;
    }

    // Create SDL_Surface that points into rgbFrame's data.
    // The surface is valid until the next getCameraFrame() call or shutdown.
    slot.sdlSurface = SDL_CreateSurfaceFrom(
        slot.rgbFrame.cols,
        slot.rgbFrame.rows,
        SDL_PIXELFORMAT_RGB24,
        slot.rgbFrame.data,
        static_cast<int>(slot.rgbFrame.step[0])
    );

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
            LOG_WARNING(VISION_LOG_ID, "saveCameraFrame: no frame available for camera {}", cameraIndex);
            return STATUS_ERROR_GENERIC;
        }
        frameCopy = f_cameras[cameraIndex]->latestFrame.clone();
    }

    // Ensure output directory exists
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
