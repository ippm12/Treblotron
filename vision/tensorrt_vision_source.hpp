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

        // Async engine build — init() returns immediately and the TRT engine
        // construction runs on m_buildThread. UI polls these to render a
        // loading screen.
        bool         isInitializing() const override;
        bool         isFailed() const override;
        float        getInitProgress() const override;
        uint64_t     getInitIteration() const override;
        std::string  getInitStatus() const override;

        std::string  getDetectionStatus() const override;

    private:
        // Forward declaration — full type lives in the .cpp so only that
        // translation unit sees TensorRT / CUDA headers.
        struct Impl;

        // State machine for the async init path.
        enum class BuildState : uint8_t
        {
            Idle,      // before init() has been called
            Building,  // engine is being loaded from cache or built from ONNX
            Ready,     // engine + inference thread are live
            Failed     // init failed — runtime disabled
        };

        void buildThreadMain();
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
         * peak + offset, gated by exist_logit), drive the Detecting/Removing
         * state machine using `handPresent`, and — when in Detecting and
         * the streak filter confirms — queue a dart event.
         */
        void handleInferenceOutputs(bool handPresent);

        std::unique_ptr<Impl> m_impl;

        // Detection mode — Detecting is the normal autoregressive flow,
        // Removing freezes dart emission and waits for the board to clear
        // (no hand AND no heatmap peak above threshold) for several
        // consecutive cycles. See the long comment in
        // tensorrt_vision_source.cpp for the full state-machine rules.
        enum class DetectionMode : uint8_t
        {
            Detecting,
            Removing
        };

        std::thread             m_thread;
        std::atomic<bool>       m_running{false};
        std::atomic<bool>       m_resetRequested{false};
        std::atomic<bool>       m_boardClear{true};

        // Async-init state — updated by m_buildThread. The UI reads these
        // via the public isInitializing / getInitProgress / getInitStatus /
        // getInitIteration accessors.
        //
        // m_buildProgress is monotonic: it advances 0 → 1 once across the
        // whole startup, driven by C++ phase markers in buildThreadMain
        // (not by TRT's per-phase reporter). m_buildIteration is the raw
        // TRT step counter, accumulated across every internal phase, shown
        // on the loading screen as "still doing something" feedback during
        // long-running phases where the bar doesn't visibly move.
        std::thread               m_buildThread;
        std::atomic<BuildState>   m_buildState{BuildState::Idle};
        std::atomic<float>        m_buildProgress{0.0f};   // 0..1, monotonic
        std::atomic<uint64_t>     m_buildIteration{0};     // raw TRT step counter
        std::atomic<bool>         m_buildAbort{false};     // set by shutdown()
        mutable std::mutex        m_buildStatusMutex;
        std::string               m_buildStatus;           // human-readable phase / error text

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
        // Cleared on resetDarts() and on entry to DetectionMode::Removing.
        std::vector<PolarDart> m_confirmedDarts;

        // ----- Hand-detection / removing-darts state ---------------------
        //
        // BlazePalm runs round-robin on one camera per cycle. Per-camera
        // last-result memory is OR'd into a single "is a hand present"
        // flag so the system stays robust when a held dart occludes the
        // hand from one or two cameras. See the explanatory comment in
        // the .cpp at the use site.
        // Atomic so the UI thread can read state for the vision_debug
        // overlay without a lock. Only the inference thread writes them,
        // so relaxed ordering is fine — nothing depends on these for
        // synchronization, only display.
        std::atomic<DetectionMode> m_mode{DetectionMode::Detecting};
        std::atomic<int>           m_handStreak{0};   // consecutive cycles handPresent (Detecting only)
        std::atomic<int>           m_clearStreak{0};  // consecutive clean cycles (Removing only)

        // Snapshot of last cycle's gating flags — exposed in the
        // detection status string so the vision_debug overlay can show
        // which condition is keeping the system stuck in Removing
        // (palm detector firing vs leftover heatmap peak).
        std::atomic<bool>          m_lastHandPresent{false};
        std::atomic<bool>          m_lastPeakAboveThresh{false};

        uint32_t                   m_palmFrameCounter = 0;  // round-robin index into the cameras
        bool                       m_palmRecent[EXPECTED_CAMERA_COUNT] = {false, false, false};

        // Cycles of palm-present required before entering Removing. Two
        // is enough to absorb a single isolated false positive from the
        // detector.
        static constexpr int HAND_ENTER_FRAMES = 2;

        // Cycles of (no hand AND no heatmap peak above threshold)
        // required before declaring the board clear and returning to
        // Detecting. ~0.33 s at the 30 Hz camera rate.
        static constexpr int CLEAR_CONFIRM_FRAMES = 10;

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
