/**
 * peak_detection.cpp
 *
 * See peak_detection.hpp for algorithm notes.
 */

#include "peak_detection.hpp"

#include <algorithm>

namespace Vision
{

static inline float sampleHeatmap(const float* h, int W, int H, int r, int c)
{
    if(r < 0 || r >= H || c < 0 || c >= W) return 0.0f;
    return h[r * W + c];
}

std::vector<HeatmapPeak> findHeatmapPeaks(const float* heatmap,
                                          uint32_t width, uint32_t height,
                                          float threshold,
                                          int minDistance)
{
    std::vector<HeatmapPeak> peaks;
    if(!heatmap || width == 0 || height == 0) return peaks;

    const int W = static_cast<int>(width);
    const int H = static_cast<int>(height);
    const int d = std::max(1, minDistance);

    for(int r = 0; r < H; r++)
    {
        for(int c = 0; c < W; c++)
        {
            float v = heatmap[r * W + c];
            if(v < threshold) continue;

            // Local-max check: strictly greater than every other neighbor.
            // Equal values at the boundary would produce duplicates, so we
            // keep only the pixel that strictly dominates its neighborhood.
            bool isPeak = true;
            for(int dr = -d; dr <= d && isPeak; dr++)
            {
                for(int dc = -d; dc <= d && isPeak; dc++)
                {
                    if(dr == 0 && dc == 0) continue;
                    float n = sampleHeatmap(heatmap, W, H, r + dr, c + dc);
                    if(n > v) { isPeak = false; break; }
                    // Tie-break: prefer top-left pixel on plateaus
                    if(n == v && (dr < 0 || (dr == 0 && dc < 0)))
                    {
                        isPeak = false;
                        break;
                    }
                }
            }
            if(!isPeak) continue;

            // Sub-pixel refinement via weighted centroid on a 3x3 window.
            // Clamp values below threshold to 0 so only strong pixels contribute.
            float wsum = 0.0f, rsum = 0.0f, csum = 0.0f;
            for(int dr = -1; dr <= 1; dr++)
            {
                for(int dc = -1; dc <= 1; dc++)
                {
                    float n = sampleHeatmap(heatmap, W, H, r + dr, c + dc) - threshold;
                    if(n <= 0.0f) continue;
                    wsum += n;
                    rsum += n * static_cast<float>(r + dr);
                    csum += n * static_cast<float>(c + dc);
                }
            }

            HeatmapPeak p;
            if(wsum > 1e-10f)
            {
                p.row = rsum / wsum;
                p.col = csum / wsum;
            }
            else
            {
                p.row = static_cast<float>(r);
                p.col = static_cast<float>(c);
            }
            p.confidence = v;
            peaks.push_back(p);
        }
    }

    return peaks;
}

}  // namespace Vision
