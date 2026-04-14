/**
 * peak_detection.hpp
 *
 * Heatmap peak finder for dart detection. Ports the algorithm used by the
 * DartModelTraining Python tooling:
 *   1. Accept pixels where heatmap[r][c] >= threshold AND is the maximum
 *      within a (2*minDistance+1) square neighborhood.
 *   2. For each surviving peak, compute a sub-pixel weighted centroid over
 *      a 3x3 window (values clamped below the threshold).
 *
 * INTERNAL HEADER — not exposed outside the vision module.
 */

#ifndef VISION_PEAK_DETECTION_HPP
#define VISION_PEAK_DETECTION_HPP

#include <cstdint>
#include <vector>

namespace Vision
{

struct HeatmapPeak
{
    float row;        // sub-pixel row in heatmap space
    float col;        // sub-pixel col in heatmap space
    float confidence; // raw heatmap value at the integer peak
};

/**
 * Find dart peaks in a 2D heatmap.
 *
 * @param heatmap   Row-major float array of length width*height, values in [0, 1].
 * @param width     Heatmap width  in pixels (columns).
 * @param height    Heatmap height in pixels (rows).
 * @param threshold Minimum heatmap value for a peak to be kept.
 * @param minDistance Half-side of the neighborhood square; a peak must be the
 *                    unique maximum within (2*minDistance+1)^2 pixels.
 */
std::vector<HeatmapPeak> findHeatmapPeaks(const float* heatmap,
                                          uint32_t width, uint32_t height,
                                          float threshold = 0.55f,
                                          int minDistance = 2);

}  // namespace Vision

#endif // VISION_PEAK_DETECTION_HPP
