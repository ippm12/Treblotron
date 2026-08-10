/**
 * dart_detector.hpp
 *
 * The shared dart-detection pipeline. Takes raw camera frames in, produces
 * confirmed dart positions out. Owns everything between those two points:
 *
 *   1. Run dart_seg_unet (batch=3, 640x360) to get a per-camera foreground mask.
 *   2. Threshold (sigmoid > 0.5), upsample to native, multiply with the raw RGB
 *      to zero out background, then warp into the 720x720 canonical view via the
 *      per-camera wire-calibration homography.
 *   3. Pack the 3 masked-warped frames + a conditioning mask of already-placed
 *      darts into a 10-channel NCHW tensor.
 *   4. Run multicam_unet_ar and decode (heatmap, exist_logit) into at most one
 *      new dart per cycle.
 *   5. Drive the Detecting/Removing state machine using BlazePalm + MediaPipe
 *      hand-landmark presence.
 *
 * This is deliberately free of any camera, window, or game dependency: the
 * game application drives it from local cameras (LocalVisionSource) and the
 * inference server drives it from frames arriving over a socket. Both get
 * bit-identical results because they run this same code.
 *
 * Requires the wire calibration to be loaded (see wire_calibration.hpp) —
 * uncalibrated cameras are skipped and contribute a zeroed slot.
 */

#ifndef DETECT_DART_DETECTOR_HPP
#define DETECT_DART_DETECTOR_HPP

#include "common_inc.hpp"
#include "detect/inference_backend.hpp"
#include "detect/wire_calibration.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace cv { class Mat; }


/** Model file names, resolved relative to DartDetectorConfig::modelDir. */
struct DartDetectorConfig
{
    std::string modelDir = "./detect/models";

    /**
     * Run the BlazePalm + hand-landmark stages. These drive the Removing
     * state (a hand reaching in means darts are being collected). Disable
     * only for offline replay, where there is no live hand to detect.
     */
    bool enableHandFilter = true;

    /**
     * Consecutive cycles a candidate peak must survive before it is promoted
     * to a confirmed dart. Tuned for the ~30 Hz camera rate of a local
     * TensorRT pipeline; a slower CPU backend may want fewer.
     */
    int confirmFrames = 3;

    /** Cycles of (no hand AND no heatmap peak) required to leave Removing. */
    int clearConfirmFrames = 10;

    /** Cycles of hand-present required to enter Removing. */
    int handEnterFrames = 2;
};


struct DartDetection
{
    float angle;            // degrees, OpenCV convention (0 = right, +CW)
    float normalizedRadius; // 0..1 at the outer double wire
};


struct DartDetectorResult
{
    /** Darts confirmed during this cycle. At most one per run(). */
    std::vector<DartDetection> newDarts;

    bool boardClear = true;

    /**
     * Human-readable state for the vision_debug overlay, e.g.
     * "Detecting  palm=0.02 hm=0.91 exist=+1.24 streak=2/3 ...".
     */
    std::string status;
};


class DartDetector
{
    public:
        DartDetector();
        ~DartDetector();

        DartDetector(const DartDetector&) = delete;
        DartDetector& operator=(const DartDetector&) = delete;

        /**
         * Load every model and allocate all buffers. Blocking — on a first-run
         * TensorRT build this takes minutes, so callers that have a UI should
         * run it on a background thread and surface `onProgress`.
         *
         * `abort` is polled throughout; set it to unwind a build early.
         */
        Status build(const DartDetectorConfig& config,
                     const InferenceBackend::ProgressFn& onProgress,
                     const std::atomic<bool>& abort);

        /**
         * Run one full inference cycle.
         *
         * `rawFrames` are native-resolution (1280x720) RGB8; an empty Mat means
         * that camera has no frame this cycle and its slot is zeroed.
         *
         * `segPlanes` is an optional fast path: when a caller already produced
         * the (3, 360, 640) NCHW float seg input for a camera — the local
         * pipeline does this on the capture threads, in parallel — pass it here
         * to skip the resize+normalize. Pass nullptr for any camera (or for the
         * whole array) to have the detector compute it inline.
         *
         * Returns false only on a hard inference failure; "no dart this cycle"
         * is a successful call with an empty result.
         */
        bool run(const std::array<cv::Mat, EXPECTED_CAMERA_COUNT>& rawFrames,
                 const float* const segPlanes[EXPECTED_CAMERA_COUNT],
                 DartDetectorResult& out);

        /**
         * Which backend the models actually ended up running on, e.g.
         * "DirectML (ONNX Runtime)" or "ONNX Runtime (CPU — DirectML
         * unavailable)". Reported after build() so it reflects any fallback
         * that happened during loading, not just the configured choice.
         * Empty before build().
         */
        std::string backendName() const;

        /** Drop all candidate/confirmed dart state and return to Detecting. */
        void reset();

        /**
         * Copy the latest sigmoid heatmap (360x360, row-major, [0, 1]).
         * Returns false before the first successful run().
         */
        bool latestHeatmap(std::vector<float>& out,
                           uint32_t& width, uint32_t& height) const;

        /**
         * The masked, warped 720x720 RGB frame for a camera from the last
         * run(). Empty when that camera contributed nothing. Used for the
         * vision_debug preview so nothing pays for a second warp.
         */
        const cv::Mat& warpedFrame(uint32_t camIndex) const;

        /** Release all models and buffers. Safe to call without build(). */
        void shutdown();

    private:
        struct Impl;
        std::unique_ptr<Impl> m_impl;
};

using DartDetectorPtr = std::shared_ptr<DartDetector>;

#endif // DETECT_DART_DETECTOR_HPP
