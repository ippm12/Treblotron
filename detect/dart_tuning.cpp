/**
 * dart_tuning.cpp
 *
 * Clamping and formatting for the user-adjustable detection settings.
 */

#include "detect/dart_tuning.hpp"

#include <algorithm>


void clampDartTuning(DartTuning& tuning)
{
    tuning.confirmFrames = std::clamp(tuning.confirmFrames,
                                      DART_CONFIRM_FRAMES_MIN, DART_CONFIRM_FRAMES_MAX);
    tuning.confirmHoldMs = std::clamp(tuning.confirmHoldMs,
                                      DART_CONFIRM_HOLD_MIN,   DART_CONFIRM_HOLD_MAX);
    tuning.clearHoldMs   = std::clamp(tuning.clearHoldMs,
                                      DART_CLEAR_HOLD_MIN,     DART_CLEAR_HOLD_MAX);
    tuning.handEnterMs   = std::clamp(tuning.handEnterMs,
                                      DART_HAND_ENTER_MIN,     DART_HAND_ENTER_MAX);
}


std::string describeVisionSettings(const DartVisionSettings& settings)
{
    const DartTuning& t = settings.tuning;
    return "confirm=" + std::to_string(t.confirmFrames)
         + "/"        + std::to_string(t.confirmHoldMs) + "ms"
         + " clear="  + std::to_string(t.clearHoldMs)   + "ms"
         + " hand="   + std::to_string(t.handEnterMs)   + "ms"
         + " capture=" + (settings.captureOnDetect ? "on" : "off");
}
