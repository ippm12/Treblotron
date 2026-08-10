/**
 * vision_link.cpp
 *
 * Storage and persistence for the inference server address. The link-health
 * half is answered by the active vision source, since only it knows what the
 * socket is doing; see vision.cpp.
 */

#include "vision/vision_link.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>


namespace
{
    // Read by the client thread on every reconnect, written by the UI thread
    // when the user edits it. Contended about once a minute, so a plain mutex
    // is the right amount of machinery.
    std::mutex  f_mutex;
    std::string f_address;
    std::string f_path = "./config/server.txt";

    /** Trim surrounding whitespace; users paste addresses with stray spaces. */
    std::string trimmed(const std::string& s)
    {
        const size_t first = s.find_first_not_of(" \t\r\n");
        if(first == std::string::npos) return {};
        const size_t last = s.find_last_not_of(" \t\r\n");
        return s.substr(first, last - first + 1);
    }
}


std::string getInferenceServerAddress()
{
    std::lock_guard<std::mutex> lock(f_mutex);
    return f_address;
}


Status setInferenceServerAddress(const std::string& hostPort)
{
    const std::string clean = trimmed(hostPort);

    std::string path;
    {
        std::lock_guard<std::mutex> lock(f_mutex);
        f_address = clean;
        path      = f_path;
    }

    std::error_code ec;
    const std::filesystem::path p(path);
    if(p.has_parent_path())
    {
        std::filesystem::create_directories(p.parent_path(), ec);
        if(ec)
        {
            LOG_ERROR(VISION_LOG_ID, "Could not create config directory: {}", ec.message());
            return STATUS_ERROR_GENERIC;
        }
    }

    std::ofstream out(path, std::ios::trunc);
    if(!out)
    {
        LOG_ERROR(VISION_LOG_ID, "Could not open {} for writing", path);
        return STATUS_ERROR_GENERIC;
    }
    out << clean << "\n";

    LOG_INFO(VISION_LOG_ID, "Inference server address set to '{}' (saved to {})", clean, path);
    return STATUS_OK;
}


Status loadInferenceServerAddress(const std::string& path)
{
    std::string fromFile;
    {
        std::ifstream in(path);
        if(in)
        {
            std::getline(in, fromFile);
            fromFile = trimmed(fromFile);
        }
    }

    // The file wins once anything has been saved through the UI. The
    // environment variable stays as a seed for scripted deployments that
    // predate the settings page.
    std::string chosen = fromFile;
    const char* env = std::getenv("DARTLENS_SERVER");
    if(chosen.empty() && env && *env)
    {
        chosen = trimmed(env);
        LOG_INFO(VISION_LOG_ID, "Using inference server '{}' from DARTLENS_SERVER", chosen);
    }

    {
        std::lock_guard<std::mutex> lock(f_mutex);
        f_address = chosen;
        f_path    = path;
    }

    if(chosen.empty())
    {
        LOG_WARNING(VISION_LOG_ID,
                    "No inference server configured — set one from the settings screen");
        return STATUS_ERROR_GENERIC;
    }

    LOG_INFO(VISION_LOG_ID, "Inference server address: {}", chosen);
    return STATUS_OK;
}
