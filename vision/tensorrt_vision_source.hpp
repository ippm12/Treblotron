/**
 * tensorrt_vision_source.hpp
 *
 * TensorRT-accelerated dart detection for Jetson (Orin Nano Super). Runs the
 * autoregressive multicam_unet_ar U-Net on three perspective-warped camera
 * frames plus a conditioning mask of already-confirmed dart tips, and
 * decodes the heatmap + offset outputs into dart events.
 *
 * The header avoids including any TensorRT / CUDA types — the handles live
 * in a pImpl owned by the .cpp, so the rest of the project (and the
 * Windows / Pi builds) never touch those SDKs.
 *
 * INTERNAL HEADER — do not include from outside the vision module.
 */

#ifndef TENSORRT_VISION_SOURCE_HPP
#define TENSORRT_VISION_SOURCE_HPP

#include "vision_source.hpp"
#include "vision/vision.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>


class TensorRTVisionSource : public VisionSource
{
    public:
        TensorRTVisionSource();
        ~TensorRTVisionSource() override;

        Status init() override;
        void tick(float deltaTime) override;
        void shutdown() override;
        bool isBoardClear() const override;
        void resetDarts() override;
        bool getLatestHeatmap(std::vector<float>& out,
                              uint32_t& width, uint32_t& height) const override;

    private:
        // Forward declaration — full type lives in the .cpp so only that
        // translation unit sees TensorRT / CUDA headers.
        struct Impl;

        void inferenceLoop();

        struct PolarDart
        {
            float angle;             // degrees
            float normalizedRadius;  // 0..1
            float templateX;         // 720-space x — kept so we can rasterize
            float templateY;         // 720-space y — the conditioning mask.
        };

        /**
         * Core detection step: decode the TRT output tensors (single argmax
         * peak + offset, gated by exist_logit), run it through the streak
         * filter, and queue a dart event on confirmation.
         */
        void handleInferenceOutputs();

        std::unique_ptr<Impl> m_impl;

        std::thread             m_thread;
        std::atomic<bool>       m_running{false};
        std::atomic<bool>       m_resetRequested{false};
        std::atomic<bool>       m_boardClear{true};

        // Temporal filter: a candidate peak must appear in CONFIRM_FRAMES
        // consecutive inferences before it's promoted to a confirmed dart
        // and emitted. Matches the behaviour of the Hailo source so the
        // game layer sees consistent dart-event timing across platforms.
        static constexpr int CONFIRM_FRAMES = 3;

        struct CandidateDart
        {
            PolarDart polar;
            int       streak = 0;
            bool      emitted = false;
        };
        std::vector<CandidateDart> m_candidates;

        // Confirmed darts — rasterized into channel 9 of the input tensor
        // each inference so the AR model only hunts for the NEXT dart.
        // Cleared on resetDarts().
        std::vector<PolarDart> m_confirmedDarts;

        // Events produced by the inference thread, consumed by tick() on
        // the main thread which forwards them through m_onDartLanded /
        // m_onDartPositionCalculated.
        std::mutex                              m_eventMutex;
        std::queue<std::pair<float, float>>     m_newDartEvents;  // (angle, normRadius)

        // Latest heatmap snapshot — written after each sigmoid, read by the
        // debug UI via getLatestHeatmap().
        mutable std::mutex      m_heatmapMutex;
        std::vector<float>      m_latestHeatmap;
        uint32_t                m_latestHeatmapW = 0;
        uint32_t                m_latestHeatmapH = 0;
};

#endif // TENSORRT_VISION_SOURCE_HPP
