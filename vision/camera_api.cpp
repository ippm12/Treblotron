/**
 * camera_api.cpp
 *
 * Stub implementations for the camera API. These return sensible
 * defaults (no cameras, no frames) until real camera support is added.
 */

#include "vision/vision.hpp"
#include <random>
#include <sstream>
#include <iomanip>

// ----------------------------------------------------------------------------
// UUID v4 generator (internal)
// ----------------------------------------------------------------------------

static std::string generateUuidV4()
{
    static std::mt19937 gen{std::random_device{}()};
    std::uniform_int_distribution<uint32_t> dist(0, 15);
    std::uniform_int_distribution<uint32_t> dist2(8, 11); // for variant bits

    const char* hex = "0123456789abcdef";
    // xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx
    constexpr int uuidLen = 36;
    constexpr const char* pattern = "xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx";

    std::string uuid(uuidLen, ' ');
    for(int i = 0; i < uuidLen; i++)
    {
        if(pattern[i] == '-')
        {
            uuid[i] = '-';
        }
        else if(pattern[i] == '4')
        {
            uuid[i] = '4';
        }
        else if(pattern[i] == 'y')
        {
            uuid[i] = hex[dist2(gen)];
        }
        else
        {
            uuid[i] = hex[dist(gen)];
        }
    }
    return uuid;
}


// ----------------------------------------------------------------------------
// Stub implementations
// ----------------------------------------------------------------------------

Status initializeCameraSystem()
{
    LOG_INFO(VISION_LOG_ID, "Camera system initialized (stub)");
    return STATUS_OK;
}


void shutdownCameraSystem()
{
    LOG_INFO(VISION_LOG_ID, "Camera system shut down (stub)");
}


uint32_t getCameraCount()
{
    return 0;
}


std::string getCameraName(uint32_t index)
{
    return "Camera " + std::to_string(index);
}


SDL_Surface* getCameraFrame(uint32_t index)
{
    (void)index;
    return nullptr;
}


Status saveCameraFrame(uint32_t cameraIndex, const std::string& outputDir)
{
    std::string uuid = generateUuidV4();
    LOG_INFO(VISION_LOG_ID, "saveCameraFrame stub: camera {} -> {}/cam{}_{}.png (no camera available)",
             cameraIndex, outputDir, cameraIndex, uuid);
    return STATUS_OK;
}
