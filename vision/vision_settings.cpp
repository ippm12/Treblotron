/**
 * vision_settings.cpp
 *
 * Storage and persistence for the detection settings. Plain "key value" lines,
 * matching wire_calibration.txt: a config a user can read, diff and fix in a
 * text editor when something has gone wrong on a machine with no keyboard.
 */

#include "vision/vision_settings.hpp"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>


namespace
{
    // Written by the UI thread on an edit, read by an inference or client
    // thread on every cycle. The generation counter is what the readers
    // actually poll; the mutex is only taken when it has changed.
    std::mutex         f_mutex;
    DartVisionSettings f_settings;
    std::string        f_path = "./config/vision.txt";

    std::atomic<uint32_t> f_generation{0};

    /** Every key the file understands, in the order they are written. */
    const char* const KEY_CONFIRM_FRAMES = "confirm_frames";
    const char* const KEY_CONFIRM_HOLD   = "confirm_hold_ms";
    const char* const KEY_CLEAR_HOLD     = "clear_hold_ms";
    const char* const KEY_HAND_ENTER     = "hand_enter_ms";
    const char* const KEY_CAPTURE        = "capture_on_detect";

    Status writeSettings(const std::string& path, const DartVisionSettings& s)
    {
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

        out << "# DartLens detection settings — edited from the Vision screen.\n";
        out << KEY_CONFIRM_FRAMES << " " << s.tuning.confirmFrames << "\n";
        out << KEY_CONFIRM_HOLD   << " " << s.tuning.confirmHoldMs << "\n";
        out << KEY_CLEAR_HOLD     << " " << s.tuning.clearHoldMs   << "\n";
        out << KEY_HAND_ENTER     << " " << s.tuning.handEnterMs   << "\n";
        out << KEY_CAPTURE        << " " << (s.captureOnDetect ? 1 : 0) << "\n";
        return out ? STATUS_OK : STATUS_ERROR_GENERIC;
    }
}


DartVisionSettings getVisionSettings()
{
    std::lock_guard<std::mutex> lock(f_mutex);
    return f_settings;
}


uint32_t getVisionSettingsGeneration()
{
    return f_generation.load(std::memory_order_acquire);
}


Status setVisionSettings(const DartVisionSettings& settings)
{
    DartVisionSettings clean = settings;
    clampDartTuning(clean.tuning);

    std::string path;
    {
        std::lock_guard<std::mutex> lock(f_mutex);
        f_settings = clean;
        path       = f_path;
    }

    // Published after the value, so a reader that sees the new generation is
    // guaranteed to read the new settings behind it.
    f_generation.fetch_add(1, std::memory_order_release);

    LOG_INFO(VISION_LOG_ID, "Vision settings: {}", describeVisionSettings(clean));
    return writeSettings(path, clean);
}


Status loadVisionSettings(const std::string& path)
{
    DartVisionSettings loaded;    // starts at the shipped defaults
    bool found = false;

    std::ifstream in(path);
    if(in)
    {
        found = true;
        std::string line;
        while(std::getline(in, line))
        {
            if(line.empty() || line[0] == '#') continue;

            std::istringstream ls(line);
            std::string key;
            long        value = 0;
            if(!(ls >> key >> value)) continue;

            // Unknown keys are skipped rather than rejected: a file written by
            // a build that knew about a knob this one does not should still
            // load everything else.
            if     (key == KEY_CONFIRM_FRAMES) loaded.tuning.confirmFrames = static_cast<int>(value);
            else if(key == KEY_CONFIRM_HOLD)   loaded.tuning.confirmHoldMs = static_cast<int>(value);
            else if(key == KEY_CLEAR_HOLD)     loaded.tuning.clearHoldMs   = static_cast<int>(value);
            else if(key == KEY_HAND_ENTER)     loaded.tuning.handEnterMs   = static_cast<int>(value);
            else if(key == KEY_CAPTURE)        loaded.captureOnDetect      = (value != 0);
        }
    }

    clampDartTuning(loaded.tuning);

    {
        std::lock_guard<std::mutex> lock(f_mutex);
        f_settings = loaded;
        f_path     = path;
    }
    f_generation.fetch_add(1, std::memory_order_release);

    if(!found)
    {
        LOG_INFO(VISION_LOG_ID, "No {} — using default vision settings: {}",
                 path, describeVisionSettings(loaded));
        return STATUS_OK;
    }

    LOG_INFO(VISION_LOG_ID, "Vision settings from {}: {}", path,
             describeVisionSettings(loaded));
    return STATUS_OK;
}
