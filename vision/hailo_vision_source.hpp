/**
 * hailo_vision_source.hpp
 *
 * HAILO-accelerated dart detection vision source. Runs a U-Net heatmap
 * model on three perspective-warped camera frames and converts peaks into
 * dart events.
 *
 * The header avoids including any HailoRT types — the real HailoRT handles
 * live in a pImpl owned by the .cpp file, so the rest of the project (and
 * the Windows build) never touch the SDK.
 *
 * INTERNAL HEADER — do not include from outside the vision module.
 */

#ifndef HAILO_VISION_SOURCE_HPP
#define HAILO_VISION_SOURCE_HPP

#include "vision_source.hpp"
#include "vision/vision.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>


class HailoVisionSource : public VisionSource
{
    public:
        HailoVisionSource();
        ~HailoVisionSource() override;

        Status init() override;
        void tick(float deltaTime) override;
        void shutdown() override;
        bool isBoardClear() const override;
        void resetDarts() override;
        bool getLatestHeatmap(std::vector<float>& out,
                              uint32_t& width, uint32_t& height) const override;

    private:
        // Forward declaration — full type lives in the .cpp so only that
        // translation unit needs to see HailoRT headers.
        struct Impl;

        void inferenceLoop();

        /**
         * Core detection step: invoked from the inference thread whenever the
         * network produces a new heatmap. Converts peaks to polar coords,
         * identifies which are new since the previous inference, and queues
         * dart events for the main thread to consume.
         */
        void handleHeatmap(const float* heatmap, uint32_t w, uint32_t h);

        struct PolarDart
        {
            float angle;             // degrees
            float normalizedRadius;  // 0..1
        };

        std::unique_ptr<Impl> m_impl;

        std::thread             m_thread;
        std::atomic<bool>       m_running{false};
        std::atomic<bool>       m_resetRequested{false};
        std::atomic<bool>       m_boardClear{true};

        // Temporal filter: a peak must appear in CONFIRM_FRAMES consecutive
        // inferences before it's reported as a dart. Each candidate tracks
        // its frame streak. Owned by the inference thread — no external access.
        static constexpr int CONFIRM_FRAMES = 3;

        struct CandidateDart
        {
            PolarDart polar;
            int       streak = 0;   // consecutive frames this peak has been seen
            bool      emitted = false;
        };

        std::vector<CandidateDart> m_candidates;

        // Previous inference's confirmed peaks for board-clear detection.
        std::vector<PolarDart>  m_prevPeaks;

        // Events produced by the inference thread, consumed by tick() on the
        // main thread which forwards them through m_onDartLanded /
        // m_onDartPositionCalculated.
        std::mutex              m_eventMutex;
        std::queue<PolarDart>   m_newDartEvents;

        // Latest heatmap snapshot — written by the inference thread after
        // each sigmoid, read by the debug UI via getLatestHeatmap().
        mutable std::mutex      m_heatmapMutex;
        std::vector<float>      m_latestHeatmap;
        uint32_t                m_latestHeatmapW = 0;
        uint32_t                m_latestHeatmapH = 0;
};

#endif // HAILO_VISION_SOURCE_HPP
