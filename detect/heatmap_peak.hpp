#pragma once

#include <cmath>
#include <cstddef>
#include <limits>

// regnetx_fast/manifest.json: max(logit(0.55), -exist_bias / exist_scale).
// The existence head was removed from the ONNX; this preserves its decisions.
inline constexpr float DART_PEAK_LOGIT_THRESHOLD = 0.2006706954621514f;

struct DartHeatmapPeak
{
    size_t index = 0;
    float logit = -std::numeric_limits<float>::infinity();
    bool present() const { return std::isfinite(logit) && logit >= DART_PEAK_LOGIT_THRESHOLD; }
};

inline DartHeatmapPeak findDartHeatmapPeak(const float* logits, size_t count)
{
    DartHeatmapPeak peak;
    for(size_t i = 0; i < count; ++i)
    {
        // Fail closed on invalid model output, including NaNs outside the peak.
        if(!std::isfinite(logits[i])) return {};
        if(logits[i] > peak.logit) peak = {i, logits[i]};
    }
    return peak;
}
