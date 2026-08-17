/**
 * local_vision_source.hpp
 *
 * Dart detection running on this machine, against the cameras attached to it.
 * Owns the threading and the camera plumbing; the actual detection pipeline
 * lives in DartDetector so it is shared verbatim with the inference server.
 *
 * Whether the forward passes land on TensorRT or on the CPU is decided by
 * DARTMATIC_INFER_BACKEND at build time and is invisible here.
 *
 * Model loading happens on a background thread — a first-run TensorRT engine
 * build takes minutes — so init() returns immediately and the main loop draws
 * a loading screen while isInitializing() is true.
 *
 * INTERNAL HEADER — do not include from outside the vision module.
 */

#ifndef LOCAL_VISION_SOURCE_HPP
#define LOCAL_VISION_SOURCE_HPP

#include "vision_source.hpp"
#include "vision/vision.hpp"
#include "detect/dart_detector.hpp"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>


class LocalVisionSource : public VisionSource
{
    public:
        LocalVisionSource();
        ~LocalVisionSource() override;

        Status init() override;
        void   tick(float deltaTime) override;
        void   shutdown() override;
        bool   isBoardClear() const override;
        void   resetDarts() override;
        bool   getLatestHeatmap(std::vector<float>& out,
                                uint32_t& width, uint32_t& height) const override;

        bool        isInitializing() const override;
        bool        isFailed() const override;
        float       getInitProgress() const override;
        uint64_t    getInitIteration() const override;
        std::string getInitStatus() const override;
        std::string getDetectionStatus() const override;

    private:
        enum class BuildState : uint8_t
        {
            Idle,      // before init() has been called
            Building,  // models are being loaded / compiled
            Ready,     // detector + inference thread are live
            Failed     // init failed — runtime disabled
        };

        void buildThreadMain();
        void inferenceLoop();

        DartDetector m_detector;

        std::thread       m_thread;
        std::atomic<bool> m_running{false};
        std::atomic<bool> m_resetRequested{false};
        std::atomic<bool> m_boardClear{true};

        /**
         * Generation of the vision settings the detector is currently running.
         * The inference loop compares it against the store's each cycle and
         * re-tunes when they differ, which keeps the settings screen off the
         * inference thread entirely.
         */
        std::atomic<uint32_t> m_tuningGeneration{0};

        // Async-init state, written by m_buildThread and polled by the UI.
        // m_buildProgress is monotonic: it advances 0 → 1 once across the whole
        // startup. m_buildIteration is the backend's raw step counter, shown on
        // the loading screen as "still doing something" feedback during long
        // phases where the bar doesn't visibly move.
        std::thread             m_buildThread;
        std::atomic<BuildState> m_buildState{BuildState::Idle};
        std::atomic<float>      m_buildProgress{0.0f};
        std::atomic<uint64_t>   m_buildIteration{0};
        std::atomic<bool>       m_buildAbort{false};
        mutable std::mutex      m_buildStatusMutex;
        std::string             m_buildStatus;

        // Latest detection status text, republished each cycle by the
        // inference thread for the vision_debug overlay.
        mutable std::mutex m_statusMutex;
        std::string        m_detectionStatus;

        // Events produced by the inference thread, drained by tick() on the
        // main thread which forwards them through the VisionSource callbacks.
        std::mutex                          m_eventMutex;
        std::queue<std::pair<float, float>> m_newDartEvents;  // (angle, normRadius)
};

#endif // LOCAL_VISION_SOURCE_HPP
