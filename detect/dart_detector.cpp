/**
 * dart_detector.cpp
 *
 * Implementation of the shared dart-detection pipeline. See dart_detector.hpp
 * for the stage-by-stage overview.
 *
 * Everything here is backend-agnostic: models are loaded and executed through
 * InferenceBackend, so this file compiles and behaves identically whether the
 * forward passes land on TensorRT or on OpenCV DNN.
 */

#include "detect/dart_detector.hpp"
#include "detect/wire_calibration.hpp"
#include "blazepalm_anchors.hpp"

#include <opencv2/core.hpp>
#include <opencv2/dnn.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <fstream>
#include <iterator>
#include <mutex>


#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif


// ============================================================================
// Constants
// ============================================================================

namespace
{
    // ----- Autoregressive dart detector (multicam_unet_ar) -----
    //
    // Model file names are joined onto DartDetectorConfig::modelDir at load
    // time. The cache suffix is part of the name so that a change to the
    // model contract can never pick up a stale compiled engine: "_v2" marked
    // the export that dropped the offset head, "_v3" the move from a single
    // conditioning channel to the 21-channel instance layout below.
    constexpr const char* ONNX_REL   = "multicam_unet_ar/multicam_unet_ar.onnx";
    constexpr const char* ENGINE_REL = "multicam_unet_ar/multicam_unet_ar_v3.trt";

    // Written next to the ONNX by DartModelTraining's exporter, naming the
    // conditioning layout that export expects. Checked before anything loads:
    // a 10-channel and a 21-channel export are the same file name, the same
    // output shapes and the same node names, so the only symptom of feeding one
    // the other's conditioning is worse detection — which is indistinguishable
    // from a bad camera angle until someone measures it. A three-month-stale
    // model already cost one long debugging session that way.
    constexpr const char* COND_JSON_REL = "multicam_unet_ar/multicam_unet_ar.cond.json";
    constexpr const char* COND_KIND     = "instance_v1";

    // Tensor names — must match the ONNX export in DartModelTraining/export.py.
    constexpr const char* INPUT_NAME  = "input";
    constexpr const char* OUT_HEATMAP = "heatmap";
    constexpr const char* OUT_EXIST   = "exist_logit";

    // Model IO shapes (must match the ONNX export).
    constexpr uint32_t INPUT_W  = 720;
    constexpr uint32_t INPUT_H  = 720;
    constexpr uint32_t OUTPUT_W = 360;
    constexpr uint32_t OUTPUT_H = 360;
    constexpr uint32_t N_CAMS   = EXPECTED_CAMERA_COUNT;

    // ----- Conditioning layout: "instance_v1", 21 channels -----------------
    //
    // How the model is told which darts have already been counted. The layout
    // is assembled in exactly one place in DartModelTraining —
    // dart_instances.conditioning_channels — and this mirrors it:
    //
    //   0..8    3 cameras x RGB, camera-major (cam0 RGB, cam1 RGB, cam2 RGB)
    //   9..17   per-dart masks, camera-major: 9 + cam*MAX_COND_DARTS + slot
    //   18..20  per-dart tip Gaussian, one per slot, shared across cameras
    //
    // The predecessor was a single channel holding a Gaussian per counted dart
    // and nothing else. Telling the model *where a counted dart's pixels are*,
    // rather than only where its tip is, is the whole point of the change: a
    // dart it can positively account for is one it cannot re-find.
    //
    // A slot's mask and its tip channel describe the same dart, and the pairing
    // is load-bearing — a mask says where a dart is but not which end is the
    // tip. Slot assignment is arbitrary but must be consistent within a forward
    // pass, so darts occupy slots in the order they were counted and keep them.
    constexpr uint32_t MAX_COND_DARTS = 3;   // tip_masks.MAX_DARTS

    constexpr uint32_t COND_MASK_BASE = 3u * N_CAMS;                          // 9
    constexpr uint32_t COND_TIP_BASE  = COND_MASK_BASE + N_CAMS * MAX_COND_DARTS;  // 18
    constexpr uint32_t COND_CHANNELS  = N_CAMS * MAX_COND_DARTS + MAX_COND_DARTS;  // 12

    constexpr uint32_t INPUT_C = COND_TIP_BASE + MAX_COND_DARTS;             // 21

    // Decode thresholds — match DartModelTraining/heatmap_utils.py.
    constexpr float EXIST_THRESHOLD   = 0.0f;   // logit gate
    constexpr float HEATMAP_THRESHOLD = 0.55f;  // sigmoid prob floor

    // Gaussian sigma for the tip channels — matches multicam_unet_ar.cond.json
    // ("mask_sigma": 5.0), which is written next to the ONNX by the exporter.
    constexpr float MASK_SIGMA     = 5.0f;
    constexpr int   MASK_RADIUS_PX = 20;  // ~4 sigma — per heatmap_dataset.py

    // A detection within this distance (template-space px) of a running
    // candidate is treated as the same dart.
    constexpr float DART_MATCH_RADIUS_PX = 16.0f;

    // Template-space geometry — shared with wire_calibration.cpp.
    constexpr float TEMPLATE_CENTER = 360.0f;
    constexpr float BOARD_RADIUS_PX = 290.0f;

    // Heatmap → template scale (720 / 360 = 2).
    constexpr float HEATMAP_TO_TEMPLATE = static_cast<float>(INPUT_W) / OUTPUT_W;

    // Accept peaks out to the catch-ring so misses just outside the double
    // wire can still be recorded.
    constexpr float MAX_DETECT_RADIUS = 1.35f;

    constexpr size_t INPUT_FLOATS   = static_cast<size_t>(INPUT_C) * INPUT_H * INPUT_W;
    constexpr size_t HEATMAP_FLOATS = static_cast<size_t>(OUTPUT_H) * OUTPUT_W;
    constexpr size_t PLANE_FLOATS   = static_cast<size_t>(INPUT_H) * INPUT_W;

    // ----- Dart segmentation U-Net (pre-stage to the AR detector) -----
    //
    // Runs once per cycle on a batch of all 3 cameras' raw 1280x720 frames
    // downscaled to 640x360. Output is a per-camera foreground logit map at
    // 360x640. We threshold (sigmoid > 0.5, equivalent to logit > 0), upsample
    // to native, multiply with the raw RGB to zero out background, and only
    // then warp the masked image into the 720x720 canonical view fed to the AR
    // detector. See models/multicam_unet_ar/README_seg.md for the contract.
    constexpr const char* SEG_ONNX_REL   = "multicam_unet_ar/dart_seg_unet.onnx";
    constexpr const char* SEG_ENGINE_REL = "multicam_unet_ar/dart_seg_unet_fp16.trt";

    constexpr const char* SEG_INPUT_NAME  = "input";
    constexpr const char* SEG_OUTPUT_NAME = "logits";

    constexpr uint32_t SEG_BATCH = N_CAMS;  // ONNX is fixed batch=3
    constexpr uint32_t SEG_C     = 3;
    constexpr uint32_t SEG_H     = 360;
    constexpr uint32_t SEG_W     = 640;

    constexpr size_t SEG_PLANE_FLOATS   = static_cast<size_t>(SEG_H) * SEG_W;
    constexpr size_t SEG_PER_CAM_FLOATS = static_cast<size_t>(SEG_C) * SEG_PLANE_FLOATS;
    constexpr size_t SEG_INPUT_FLOATS   = static_cast<size_t>(SEG_BATCH) * SEG_PER_CAM_FLOATS;
    constexpr size_t SEG_OUTPUT_FLOATS  = static_cast<size_t>(SEG_BATCH) * SEG_PLANE_FLOATS;

    // Native camera resolution. Used as the warp source size after masking.
    constexpr uint32_t NATIVE_W = 1280;
    constexpr uint32_t NATIVE_H = 720;

    // ----- BlazePalm (presence-only hand detector) -----
    //
    // Round-robin one camera per cycle. Per-camera last-result memory is OR'd
    // to decide "hand present" so a held dart occluding one or two camera
    // angles doesn't keep us out of the Removing state.
    constexpr const char* PALM_ONNX_REL = "palm_detection/blazepalm.onnx";
    // FP32 cache path: the full BlazePalm overflows FP16 and produces NaN, so
    // the palm engine has to be built in FP32.
    constexpr const char* PALM_ENGINE_REL = "palm_detection/blazepalm_fp32.trt";

    // Canonical I/O names. tf2onnx and most community converters spit out
    // generic names ("input_1" / "Identity" / "Identity_1") that depend on
    // TFLite output ordering and shift between exports. We rewrite them to
    // these stable names with models/palm_detection/rename_io.py — run it once
    // after each fresh ONNX export. Loading asserts on mismatch so a forgotten
    // rename can never silent-fail.
    constexpr const char* PALM_INPUT_NAME = "input";
    constexpr const char* PALM_OUT_SCORES = "classificators";
    constexpr const char* PALM_OUT_BOXES  = "regressors";

    constexpr uint32_t PALM_INPUT_W   = 192;
    constexpr uint32_t PALM_INPUT_H   = 192;
    constexpr uint32_t PALM_INPUT_C   = 3;
    constexpr uint32_t PALM_N_ANCHORS = 2016;
    constexpr uint32_t PALM_BOX_DIM   = 18;

    constexpr size_t PALM_INPUT_FLOATS  = static_cast<size_t>(PALM_INPUT_C)
                                        * PALM_INPUT_H * PALM_INPUT_W;
    constexpr size_t PALM_SCORES_FLOATS = PALM_N_ANCHORS;
    constexpr size_t PALM_BOXES_FLOATS  = static_cast<size_t>(PALM_N_ANCHORS) * PALM_BOX_DIM;

    // Sigmoid threshold on the max anchor score. BlazePalm's nominal 0.5
    // default is calibrated for natural scenes — on a static dartboard it
    // produces enough single-anchor noise spikes to keep the system stuck in
    // Removing (false positives reset the clear streak). 0.7 filters those
    // without losing recall on real hands, which typically score 0.9+.
    constexpr float PALM_PRESENCE_THRESHOLD = 0.7f;

    // ----- MediaPipe hand-landmark detector (FP filter for BlazePalm) -----
    //
    // BlazePalm produces high-confidence false positives on dart shapes
    // sitting in the bullseye region — close enough in score to real hands
    // that no single threshold separates them. The landmark stage tries to fit
    // 21 hand keypoints to the palm-detected ROI and emits a `presence` score
    // that collapses to ~0 when no plausible hand skeleton fits. We run it
    // conditionally (only when palm already passes) and AND the two results.
    //
    // Empirical reference (verify_landmark_filter.py on a no-hand dartboard):
    // landmark presence = 0.030 vs ~0.95+ for a real hand.
    constexpr const char* LANDMARK_ONNX_REL   = "palm_detection/hand_landmark.onnx";
    constexpr const char* LANDMARK_ENGINE_REL = "palm_detection/hand_landmark_fp32.trt";

    constexpr const char* LANDMARK_INPUT_NAME     = "input";
    constexpr const char* LANDMARK_OUT_LANDMARKS  = "landmarks";
    constexpr const char* LANDMARK_OUT_PRESENCE   = "presence";
    constexpr const char* LANDMARK_OUT_HANDEDNESS = "handedness";
    constexpr const char* LANDMARK_OUT_WORLD      = "world_landmarks";

    constexpr uint32_t LANDMARK_INPUT_W = 224;
    constexpr uint32_t LANDMARK_INPUT_H = 224;
    constexpr uint32_t LANDMARK_INPUT_C = 3;

    constexpr size_t LANDMARK_INPUT_FLOATS      = static_cast<size_t>(LANDMARK_INPUT_C)
                                                * LANDMARK_INPUT_H * LANDMARK_INPUT_W;
    constexpr size_t LANDMARK_LANDMARKS_FLOATS  = 63;  // 21 keypoints x (x,y,z)
    constexpr size_t LANDMARK_PRESENCE_FLOATS   = 1;
    constexpr size_t LANDMARK_HANDEDNESS_FLOATS = 1;
    constexpr size_t LANDMARK_WORLD_FLOATS      = 63;

    constexpr float LANDMARK_PRESENCE_THRESHOLD = 0.5f;

    // BlazePalm regressor decoding constants. The 18-float per-anchor
    // regression layout — verified against MediaPipe's
    // tflite_tensors_to_detections_calculator + verify_landmark_filter.py:
    //   [0..1] : (dx, dy)  bbox center offset from anchor (input pixels)
    //   [2..3] : (w,  h)   bbox size                       (input pixels)
    //   [4..17]: 7 keypoints x (x, y) anchor-center-relative (input pixels)
    constexpr uint32_t PALM_KP_WRIST  = 0;  // wrist center
    constexpr uint32_t PALM_KP_MIDDLE = 2;  // middle-finger MCP

    // Landmark crop: rotate so wrist→middle-finger axis points up, then expand
    // the long side of the palm bbox by this factor. Matches MediaPipe's
    // hand_detection_to_roi config (scale_x=2.6, scale_y=2.6).
    constexpr float LANDMARK_CROP_EXPANSION = 2.6f;
    constexpr float LANDMARK_CROP_SHIFT_Y   = -0.5f;


    /** Join a model directory and a relative model path. */
    std::string joinPath(const std::string& dir, const char* rel)
    {
        if(dir.empty()) return std::string(rel);
        if(dir.back() == '/' || dir.back() == '\\') return dir + rel;
        return dir + "/" + rel;
    }


    /**
     * Verify the AR export declares the conditioning layout this file builds.
     *
     * Deliberately a substring scan rather than a JSON parse: the sidecar is
     * four lines emitted by one known writer, and pulling a JSON dependency
     * into detect/ to read it would cost more than it protects. If the file
     * ever grows structure, this becomes wrong in the safe direction — it stops
     * finding what it needs and refuses to load.
     */
    bool conditioningMatches(const std::string& path)
    {
        std::ifstream in(path, std::ios::binary);
        if(!in)
        {
            LOG_ERROR(DETECT_LOG_ID,
                      "missing {} — this build needs the '{}' conditioning layout, and "
                      "without the sidecar there is no way to tell what the ONNX beside "
                      "it expects. Re-export from DartModelTraining.", path, COND_KIND);
            return false;
        }

        const std::string text((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());

        const std::string wanted = std::string("\"") + COND_KIND + "\"";
        if(text.find("\"cond_kind\"") == std::string::npos
        || text.find(wanted) == std::string::npos)
        {
            LOG_ERROR(DETECT_LOG_ID,
                      "{} does not declare cond_kind {} — the ONNX beside it was trained "
                      "on a different conditioning layout than this build packs. "
                      "Re-export the autoregressive model. Sidecar says: {}",
                      path, wanted, text);
            return false;
        }

        // Sigma is the other half of the contract and is one number; a mismatch
        // would blur or sharpen every tip channel relative to training.
        const size_t sigmaKey = text.find("\"mask_sigma\"");
        if(sigmaKey != std::string::npos)
        {
            const size_t digit = text.find_first_of("0123456789", sigmaKey);
            const double declared = (digit == std::string::npos)
                                  ? -1.0
                                  : std::strtod(text.c_str() + digit, nullptr);
            if(std::fabs(declared - MASK_SIGMA) > 1e-6)
            {
                LOG_ERROR(DETECT_LOG_ID,
                          "{} declares mask_sigma {} but this build rasterizes {}",
                          path, declared, MASK_SIGMA);
                return false;
            }
        }

        LOG_INFO(DETECT_LOG_ID, "AR conditioning: {} ({} channels, sigma {})",
                 COND_KIND, INPUT_C, MASK_SIGMA);
        return true;
    }


    /**
     * Rasterize a Gaussian spot at (cx, cy) into a 720x720 float plane using
     * element-wise max — mirrors heatmap_dataset.py:generate_heatmap.
     *
     * Quantized to 1/255 on the way in. That is not rounding for its own sake:
     * dart_instances.conditioning_channels builds the whole block as uint8 and
     * the caller divides by 255, precisely so that training and every consumer
     * see bit-identical values instead of one quantising and the other not.
     * Truncating (not rounding) matches numpy's astype(np.uint8).
     *
     * The window is +/-4 sigma, and the integer origin comes from truncating
     * the centre rather than rounding it, again to match generate_heatmap's
     * int(hy) - radius.
     */
    void addGaussianToPlane(float* plane, float cx, float cy)
    {
        const int ix = static_cast<int>(cx);
        const int iy = static_cast<int>(cy);
        const int x0 = std::max(0, ix - MASK_RADIUS_PX);
        const int y0 = std::max(0, iy - MASK_RADIUS_PX);
        const int x1 = std::min(static_cast<int>(INPUT_W), ix + MASK_RADIUS_PX + 1);
        const int y1 = std::min(static_cast<int>(INPUT_H), iy + MASK_RADIUS_PX + 1);
        const float twoSigmaSq = 2.0f * MASK_SIGMA * MASK_SIGMA;

        for(int y = y0; y < y1; y++)
        {
            float* row = plane + static_cast<size_t>(y) * INPUT_W;
            const float dy = static_cast<float>(y) - cy;
            for(int x = x0; x < x1; x++)
            {
                const float dx = static_cast<float>(x) - cx;
                const float g  = std::exp(-(dx * dx + dy * dy) / twoSigmaSq);
                const float q  = std::trunc(g * 255.0f) * (1.0f / 255.0f);
                if(q > row[x]) row[x] = q;
            }
        }
    }


    /**
     * The connected blob of `mask` under (cx, cy), as a 0/255 mask.
     *
     * The tip is where the model put the dart's point, which sits at the very
     * edge of its silhouette and lands a pixel or two outside it often enough
     * to matter — hence the small search window rather than a single sample.
     * Mirrors silhouette_geom._component_at, which exists for the same reason.
     *
     * Returns false when there is no mask nearby at all.
     */
    bool componentUnderTip(const cv::Mat& mask, float cx, float cy, cv::Mat& out)
    {
        constexpr int SEARCH_PX = 6;

        cv::Mat labels, stats, centroids;
        const int n = cv::connectedComponentsWithStats(mask, labels, stats, centroids, 8);
        if(n <= 1) return false;

        const int tx = static_cast<int>(std::lround(cx));
        const int ty = static_cast<int>(std::lround(cy));

        int   best  = 0;
        float bestD = static_cast<float>(SEARCH_PX * SEARCH_PX + 1);
        for(int y = std::max(0, ty - SEARCH_PX);
                y < std::min(labels.rows, ty + SEARCH_PX + 1); y++)
        {
            for(int x = std::max(0, tx - SEARCH_PX);
                    x < std::min(labels.cols, tx + SEARCH_PX + 1); x++)
            {
                const int label = labels.at<int>(y, x);
                if(label == 0) continue;
                const float dx = static_cast<float>(x - tx);
                const float dy = static_cast<float>(y - ty);
                const float d2 = dx * dx + dy * dy;
                if(d2 < bestD) { bestD = d2; best = label; }
            }
        }
        if(best == 0) return false;

        cv::compare(labels, best, out, cv::CMP_EQ);
        return true;
    }


    /**
     * Expand a 0/255 binary mask into a float plane of 0.0 / 1.0.
     *
     * The training block is uint8 divided by 255, so a set pixel is exactly
     * 1.0f — worth spelling out rather than multiplying, because 255/255 in
     * float is exact but only by luck of the value.
     */
    void packMaskPlane(const cv::Mat& mask, float* plane)
    {
        for(uint32_t r = 0; r < INPUT_H; r++)
        {
            const uint8_t* s = mask.ptr(r);
            float*         d = plane + static_cast<size_t>(r) * INPUT_W;
            for(uint32_t c = 0; c < INPUT_W; c++)
            {
                d[c] = s[c] ? 1.0f : 0.0f;
            }
        }
    }


    /** Pack an HWC-interleaved RGB8 Mat into an NHWC float32 [0, 1] buffer. */
    void packNhwcNormalized(const cv::Mat& src, float* dst, uint32_t w, uint32_t h)
    {
        constexpr float INV_255 = 1.0f / 255.0f;
        const uint32_t bytesPerRow = w * 3u;
        for(uint32_t r = 0; r < h; r++)
        {
            const uint8_t* s = src.ptr(r);
            float*         d = dst + static_cast<size_t>(r) * bytesPerRow;
            for(uint32_t b = 0; b < bytesPerRow; b++)
            {
                d[b] = static_cast<float>(s[b]) * INV_255;
            }
        }
    }
}


// ============================================================================
// pImpl
// ============================================================================

struct DartDetector::Impl
{
    DartDetectorConfig  config;
    InferenceBackendPtr backend;
    std::string         backendName;

    /**
     * The live thresholds, seeded from config.tuning at build().
     *
     * Held as atomics rather than read off `config` because the settings screen
     * writes them from the UI thread while the state machine below reads them
     * from an inference thread. They are independent scalars — nothing sizes a
     * buffer from them — so per-field atomics are the whole synchronisation
     * story; a lock would only serialise the hot loop against an edit that
     * happens once a month.
     */
    std::atomic<int> confirmFrames{DartTuning{}.confirmFrames};
    std::atomic<int> confirmHoldMs{DartTuning{}.confirmHoldMs};
    std::atomic<int> clearHoldMs  {DartTuning{}.clearHoldMs};
    std::atomic<int> handEnterMs  {DartTuning{}.handEnterMs};

    std::unique_ptr<InferenceBackend::Model> dart;
    std::unique_ptr<InferenceBackend::Model> seg;
    std::unique_ptr<InferenceBackend::Model> palm;
    std::unique_ptr<InferenceBackend::Model> landmark;

    // Detection mode — Detecting is the normal autoregressive flow, Removing
    // freezes dart emission and waits for the board to clear (no hand AND no
    // heatmap peak above threshold) for several consecutive cycles.
    enum class Mode : uint8_t { Detecting, Removing };

    struct PolarDart
    {
        float angle;            // degrees
        float normalizedRadius; // 0..1
        float templateX;        // 720-space x — kept so we can rasterize
        float templateY;        // 720-space y — the conditioning mask.
    };

    struct CandidateDart
    {
        PolarDart polar;
        int       streak  = 0;
        bool      emitted = false;
        /** When this candidate first appeared, for the confirmHoldMs gate. */
        std::chrono::steady_clock::time_point firstSeen{};
    };

    Mode                       mode = Mode::Detecting;
    std::vector<CandidateDart> candidates;

    /**
     * A dart that has been counted, and the pixels it owns in each camera.
     *
     * The mask is captured once, at the moment of confirmation, and never
     * recomputed. It has to be: it is the difference between the segmenter's
     * output now and its output before this dart landed, and that earlier
     * output is gone by the next cycle.
     */
    struct ConfirmedDart
    {
        PolarDart polar;
        /** 720x720 CV_8UC1, 0/255. Empty for a camera that had no frame. */
        cv::Mat   instance[N_CAMS];
    };

    // Confirmed darts — written into the conditioning channels of the input
    // tensor each cycle so the AR model only hunts for the NEXT dart. Cleared
    // on reset() and on entry to Removing.
    std::vector<ConfirmedDart> confirmedDarts;

    /**
     * Per camera, the union of every warped seg mask captured at a
     * confirmation so far — M_j in the training derivation.
     *
     * A new dart's own mask is the current seg output minus this, which is the
     * only way to recover one dart from a segmenter that emits every dart at
     * once. Kept as a running union rather than "the last snapshot" to match
     * build_conditioning, and because a union cannot shrink when a frame drops
     * a dart the segmenter previously found.
     */
    cv::Mat condUnion[N_CAMS];

    /** This cycle's warped binary seg mask, filled lazily on a confirmation. */
    cv::Mat maskWarped[N_CAMS];

    bool boardClear = true;

    // Wall-clock, not cycle counts: the cycle rate is a property of the backend
    // (~4 Hz on CPU, 30-55 Hz on a GPU), so counting cycles made the real
    // timing swing by 8x depending on what hardware happened to be serving.
    using Clock = std::chrono::steady_clock;

    /** When the board last had a hand in it, or a dart peak still showing. */
    Clock::time_point lastBusy = Clock::now();

    /** When the current run of hand-present began; reset when the hand goes. */
    Clock::time_point handSince{};
    bool              handWasPresent = false;

    // Round-robin palm state. A held dart can occlude the hand from one or two
    // cameras — keeping per-camera memory and OR-ing the slots makes the state
    // machine robust against that, which a "consecutive frames" rule over the
    // raw round-robin output would not be.
    uint32_t palmFrameCounter = 0;
    bool     palmRecent[EXPECTED_CAMERA_COUNT] = {false, false, false};

    // Diagnostics surfaced through DartDetectorResult::status. Watching these
    // live with hand/dart in and out of frame is how PALM_PRESENCE_THRESHOLD
    // and HEATMAP_THRESHOLD get tuned.
    bool  lastHandPresent     = false;
    bool  lastPeakAboveThresh = false;
    float lastPalmScore       = -1.0f;
    float lastHeatmapPeak     = -1.0f;
    float lastLandmarkPresence = -1.0f;
    float lastExistLogit      = -1e30f;
    int   lastCandidateStreak = 0;
    float palmTop1 = 0.0f, palmTop2 = 0.0f, palmTop3 = 0.0f;

    // Per-camera scratch, recycled each cycle so we don't reallocate.
    cv::Mat maskNative[N_CAMS];  // 1280x720 CV_8UC1 binary mask
    cv::Mat masked[N_CAMS];      // 1280x720 RGB8, raw masked by mask
    cv::Mat warped[N_CAMS];      // 720x720   RGB8, masked + warped
    cv::Mat arBlob[N_CAMS];      // (1,3,720,720) CV_32F — blobFromImage dst
    cv::Mat segBlob;             // (1,3,360,640) CV_32F — inline seg prep
    cv::Mat palmResized;         // 192x192 RGB8
    cv::Mat landmarkCropRgb;     // 224x224 RGB8

    // Latest heatmap snapshot — written after each decode, read by the debug UI.
    mutable std::mutex heatmapMutex;
    std::vector<float> latestHeatmap;
    uint32_t           latestHeatmapW = 0;
    uint32_t           latestHeatmapH = 0;

    static const cv::Mat s_emptyMat;

    std::string formatStatus() const;
    bool        runPalmStage(const std::array<cv::Mat, N_CAMS>& rawFrames);
    void        decodeAndTrack(bool handPresent, DartDetectorResult& out);
    void        captureInstanceMasks(ConfirmedDart& dart);
    void        clearConditioning();
};

const cv::Mat DartDetector::Impl::s_emptyMat;


// ============================================================================
// Lifecycle
// ============================================================================

DartDetector::DartDetector() : m_impl(std::make_unique<Impl>()) {}

DartDetector::~DartDetector() { shutdown(); }


Status DartDetector::build(const DartDetectorConfig& config,
                           const InferenceBackend::ProgressFn& onProgress,
                           const std::atomic<bool>& abort)
{
    m_impl->config = config;
    applyTuning(config.tuning);

    auto report = [&](float pct, const std::string& phase)
    {
        if(onProgress) onProgress(pct, 0, phase);
    };

    // Checked before the backend is even created: a contract mismatch is not
    // something to discover after a multi-minute engine build.
    if(!conditioningMatches(joinPath(config.modelDir, COND_JSON_REL)))
    {
        return STATUS_ERROR_GENERIC;
    }

    m_impl->backend = createInferenceBackend();
    if(!m_impl->backend)
    {
        LOG_ERROR(DETECT_LOG_ID,
                  "DartDetector::build — no inference backend compiled in "
                  "(configure with -DDARTMATIC_INFER_BACKEND=tensorrt|cpu)");
        return STATUS_ERROR_GENERIC;
    }

    if(IS_STATUS_NOT_OK(m_impl->backend->init()))
    {
        LOG_ERROR(DETECT_LOG_ID, "DartDetector: backend init failed");
        return STATUS_ERROR_GENERIC;
    }

    // Deliberately no backend name here: a backend cannot know whether it got
    // the accelerator it asked for until init() has probed and every model has
    // loaded. Naming it earlier printed "CPU" for a run that ended up on the
    // GPU. The single authoritative report is the "ready on {}" line below.
    LOG_INFO(DETECT_LOG_ID, "DartDetector: loading models from {}", config.modelDir);

    // Forward the backend's own load progress into our phase window so a long
    // TensorRT compile still moves the bar and ticks the iteration counter.
    auto sub = [&](float lo, float hi, const char* phase) -> InferenceBackend::ProgressFn
    {
        return [onProgress, lo, hi, phase](float p, uint64_t iter, const std::string& detail)
        {
            if(!onProgress) return;
            onProgress(lo + (hi - lo) * std::clamp(p, 0.0f, 1.0f), iter,
                       detail.empty() ? std::string(phase) : detail);
        };
    };

    // ---- Dart detector -------------------------------------------------
    report(0.02f, "Loading dart detector");
    {
        InferenceBackend::ModelSpec spec;
        spec.onnxPath   = joinPath(config.modelDir, ONNX_REL);
        spec.cachePath  = joinPath(config.modelDir, ENGINE_REL);
        spec.preferFp16 = true;
        // Pin the exist_logit head to FP32. The head is
        // scale * max(heatmap_logits) + bias; a ReduceMax cannot overflow the
        // way the old GlobalAveragePool over 129,600 FP16 activations could,
        // but it is four trivial ops and DartModelTraining/CHANGES.md warns
        // that FP16 was never simulated for this head. Cheap insurance.
        spec.fp32Layers = {
            "/m/exist_head/Flatten",
            "/m/exist_head/ReduceMax",
            "/m/exist_head/Mul",
            "/m/exist_head/Add",
        };
        spec.inputs  = {{INPUT_NAME, {1, static_cast<int>(INPUT_C),
                                      static_cast<int>(INPUT_H), static_cast<int>(INPUT_W)}}};
        spec.outputs = {{OUT_HEATMAP, {1, static_cast<int>(OUTPUT_H), static_cast<int>(OUTPUT_W)}},
                        {OUT_EXIST,   {1}}};

        m_impl->dart = m_impl->backend->load(spec, sub(0.02f, 0.42f, "Building dart detector"), abort);
        if(!m_impl->dart) return STATUS_ERROR_GENERIC;
    }
    if(abort.load(std::memory_order_acquire)) return STATUS_ERROR_GENERIC;

    // ---- Dart segmentation ---------------------------------------------
    report(0.43f, "Loading dart segmentation");
    {
        InferenceBackend::ModelSpec spec;
        spec.onnxPath   = joinPath(config.modelDir, SEG_ONNX_REL);
        spec.cachePath  = joinPath(config.modelDir, SEG_ENGINE_REL);
        // FP16 throughout — the seg output is a per-pixel logit we threshold
        // immediately, so quantization noise can't poison anything downstream.
        spec.preferFp16 = true;
        spec.inputs  = {{SEG_INPUT_NAME, {static_cast<int>(SEG_BATCH), static_cast<int>(SEG_C),
                                          static_cast<int>(SEG_H), static_cast<int>(SEG_W)}}};
        spec.outputs = {{SEG_OUTPUT_NAME, {static_cast<int>(SEG_BATCH), 1,
                                           static_cast<int>(SEG_H), static_cast<int>(SEG_W)}}};

        m_impl->seg = m_impl->backend->load(spec, sub(0.43f, 0.60f, "Building dart segmentation"), abort);
        if(!m_impl->seg) return STATUS_ERROR_GENERIC;
    }
    if(abort.load(std::memory_order_acquire)) return STATUS_ERROR_GENERIC;

    if(config.enableHandFilter)
    {
        // ---- BlazePalm --------------------------------------------------
        // NHWC input: the ONNX is [1, 192, 192, 3] — channels last, NOT the
        // NCHW convention the dart model uses. The graph's first op is a
        // Transpose to NCHW for the internal convs; feeding planar CHW mangles
        // the data and the network produces moderate-confidence noise
        // everywhere.
        report(0.62f, "Loading palm detector");
        {
            InferenceBackend::ModelSpec spec;
            spec.onnxPath  = joinPath(config.modelDir, PALM_ONNX_REL);
            spec.cachePath = joinPath(config.modelDir, PALM_ENGINE_REL);
            // Force FP32 — the full BlazePalm's deeper activation chain
            // overflows FP16 and produces NaN scores.
            spec.preferFp16 = false;
            spec.inputs  = {{PALM_INPUT_NAME, {1, static_cast<int>(PALM_INPUT_H),
                                               static_cast<int>(PALM_INPUT_W),
                                               static_cast<int>(PALM_INPUT_C)}}};
            spec.outputs = {{PALM_OUT_SCORES, {1, static_cast<int>(PALM_N_ANCHORS), 1}},
                            {PALM_OUT_BOXES,  {1, static_cast<int>(PALM_N_ANCHORS),
                                               static_cast<int>(PALM_BOX_DIM)}}};

            m_impl->palm = m_impl->backend->load(spec, sub(0.62f, 0.88f, "Building palm detector"), abort);
            if(!m_impl->palm) return STATUS_ERROR_GENERIC;
        }
        if(abort.load(std::memory_order_acquire)) return STATUS_ERROR_GENERIC;

        // ---- Hand landmark ----------------------------------------------
        report(0.90f, "Loading hand-landmark detector");
        {
            InferenceBackend::ModelSpec spec;
            spec.onnxPath  = joinPath(config.modelDir, LANDMARK_ONNX_REL);
            spec.cachePath = joinPath(config.modelDir, LANDMARK_ENGINE_REL);
            // FP32 to match the palm engine's caution. The net is small so the
            // cost over FP16 is negligible next to the U-Net pipeline.
            spec.preferFp16 = false;
            spec.inputs  = {{LANDMARK_INPUT_NAME, {1, static_cast<int>(LANDMARK_INPUT_H),
                                                   static_cast<int>(LANDMARK_INPUT_W),
                                                   static_cast<int>(LANDMARK_INPUT_C)}}};
            spec.outputs = {{LANDMARK_OUT_LANDMARKS,  {1, static_cast<int>(LANDMARK_LANDMARKS_FLOATS)}},
                            {LANDMARK_OUT_PRESENCE,   {1, 1}},
                            {LANDMARK_OUT_HANDEDNESS, {1, 1}},
                            {LANDMARK_OUT_WORLD,      {1, static_cast<int>(LANDMARK_WORLD_FLOATS)}}};

            m_impl->landmark = m_impl->backend->load(spec, sub(0.90f, 0.99f, "Building hand-landmark detector"), abort);
            if(!m_impl->landmark) return STATUS_ERROR_GENERIC;
        }
    }
    else
    {
        LOG_INFO(DETECT_LOG_ID, "DartDetector: hand filter disabled — "
                                "palm/landmark stages will not run");
    }

    if(abort.load(std::memory_order_acquire)) return STATUS_ERROR_GENERIC;

    // Read the name only now: a backend that tried for an accelerator and had
    // to fall back reports that through name(), and it can only know once every
    // model has been loaded.
    m_impl->backendName = m_impl->backend->name();

    report(1.0f, "Ready");
    LOG_INFO(DETECT_LOG_ID, "DartDetector ready on {}", m_impl->backendName);
    return STATUS_OK;
}


std::string DartDetector::backendName() const
{
    return m_impl->backendName;
}


void DartDetector::shutdown()
{
    if(!m_impl) return;
    m_impl->landmark.reset();
    m_impl->palm.reset();
    m_impl->seg.reset();
    m_impl->dart.reset();
    if(m_impl->backend)
    {
        m_impl->backend->shutdown();
        m_impl->backend.reset();
    }
}


void DartDetector::applyTuning(const DartTuning& tuning)
{
    // Clamped here as well as at every caller: this is the last gate before the
    // values reach the state machine, and it is reached from a config file, a
    // settings screen, a command line and a network peer.
    DartTuning t = tuning;
    clampDartTuning(t);

    m_impl->confirmFrames.store(t.confirmFrames, std::memory_order_relaxed);
    m_impl->confirmHoldMs.store(t.confirmHoldMs, std::memory_order_relaxed);
    m_impl->clearHoldMs  .store(t.clearHoldMs,   std::memory_order_relaxed);
    m_impl->handEnterMs  .store(t.handEnterMs,   std::memory_order_relaxed);
}


void DartDetector::reset()
{
    m_impl->candidates.clear();
    m_impl->clearConditioning();
    m_impl->mode        = Impl::Mode::Detecting;
    m_impl->handWasPresent = false;
    m_impl->lastBusy       = Impl::Clock::now();
    m_impl->boardClear  = true;
    for(uint32_t i = 0; i < N_CAMS; i++) m_impl->palmRecent[i] = false;
}


bool DartDetector::latestHeatmap(std::vector<float>& out,
                                 uint32_t& width, uint32_t& height) const
{
    std::lock_guard<std::mutex> lock(m_impl->heatmapMutex);
    if(m_impl->latestHeatmap.empty()) return false;
    out    = m_impl->latestHeatmap;
    width  = m_impl->latestHeatmapW;
    height = m_impl->latestHeatmapH;
    return true;
}


const cv::Mat& DartDetector::warpedFrame(uint32_t camIndex) const
{
    if(camIndex >= N_CAMS) return Impl::s_emptyMat;
    return m_impl->warped[camIndex];
}


// ============================================================================
// Inference
// ============================================================================

bool DartDetector::run(const std::array<cv::Mat, EXPECTED_CAMERA_COUNT>& rawFrames,
                       const float* const segPlanes[EXPECTED_CAMERA_COUNT],
                       DartDetectorResult& out)
{
    out.newDarts.clear();

    if(!m_impl->dart || !m_impl->seg)
    {
        return false;
    }

    // ----- Which cameras are usable this cycle? -------------------------
    uint32_t contributing = 0;
    bool     usable[N_CAMS] = {};
    for(uint32_t i = 0; i < N_CAMS; i++)
    {
        m_impl->warped[i] = cv::Mat();
        usable[i] = !rawFrames[i].empty() && isCameraCalibrated(i);
        if(usable[i]) contributing++;
    }
    if(contributing == 0)
    {
        return false;
    }

    // ----- Seg pre-stage: fill the batched NCHW input --------------------
    // Callers that already produced the prepped plane (the local pipeline
    // does it on the capture threads, in parallel with each other) hand it
    // straight in; otherwise we resize + normalize here. Missing cameras get
    // a zero plane — the seg model has fixed batch=3.
    float* segIn = m_impl->seg->input(0);
    for(uint32_t cam = 0; cam < N_CAMS; cam++)
    {
        float* dst = segIn + cam * SEG_PER_CAM_FLOATS;
        if(!usable[cam])
        {
            std::memset(dst, 0, SEG_PER_CAM_FLOATS * sizeof(float));
            continue;
        }

        const float* prepped = segPlanes ? segPlanes[cam] : nullptr;
        if(prepped)
        {
            std::memcpy(dst, prepped, SEG_PER_CAM_FLOATS * sizeof(float));
            continue;
        }

        // Inline prep — matches dart_seg_dataset.py: resize to 640x360
        // (INTER_LINEAR), /255, HWC→CHW. Frames are already RGB.
        cv::dnn::blobFromImage(rawFrames[cam], m_impl->segBlob,
                               /*scalefactor*/ 1.0 / 255.0,
                               /*size*/        cv::Size(static_cast<int>(SEG_W),
                                                        static_cast<int>(SEG_H)),
                               /*mean*/        cv::Scalar(),
                               /*swapRB*/      false,
                               /*crop*/        false,
                               CV_32F);
        std::memcpy(dst, m_impl->segBlob.ptr<float>(), SEG_PER_CAM_FLOATS * sizeof(float));
    }

    if(!m_impl->seg->run())
    {
        LOG_WARNING(DETECT_LOG_ID, "seg inference failed");
        return false;
    }

    // ----- Per-camera mask → masked RGB → warp ---------------------------
    // The mask is the sigmoid > 0.5 threshold on the seg logits, equivalent to
    // a direct logit > 0 test (sigmoid is monotonic) — saves a per-pixel
    // exp(). copyTo(dst, mask) zeroes non-mask pixels in dst, which is the
    // same result as RGB x binary without building a 3-channel mask.
    const float* segOut = m_impl->seg->output(0);
    for(uint32_t cam = 0; cam < N_CAMS; cam++)
    {
        if(!usable[cam]) continue;

        // Wrap the cam-th seg output plane (360x640 FP32 logits) with no copy.
        cv::Mat logits360(static_cast<int>(SEG_H), static_cast<int>(SEG_W), CV_32FC1,
                          const_cast<float*>(segOut) + cam * SEG_PLANE_FLOATS);

        cv::Mat mask360u8;
        cv::compare(logits360, 0.0, mask360u8, cv::CMP_GT);
        cv::resize(mask360u8, m_impl->maskNative[cam],
                   cv::Size(static_cast<int>(NATIVE_W), static_cast<int>(NATIVE_H)),
                   0, 0, cv::INTER_NEAREST);

        // setTo(0) first so previously masked-in pixels from a prior frame
        // don't leak through when the Mat is reused.
        m_impl->masked[cam].create(rawFrames[cam].size(), rawFrames[cam].type());
        m_impl->masked[cam].setTo(cv::Scalar::all(0));
        rawFrames[cam].copyTo(m_impl->masked[cam], m_impl->maskNative[cam]);

        if(!warpCameraFrame(cam, m_impl->masked[cam], m_impl->warped[cam])
        || m_impl->warped[cam].empty())
        {
            m_impl->warped[cam] = cv::Mat();
        }
    }

    // ----- Build the 21-channel NCHW input tensor -------------------------
    // Channels 0..8:   camera[c]'s RGB plane (/255 normalization).
    // Channels 9..20:  the conditioning block (see COND_MASK_BASE above) —
    //                  held all-zero while Removing so the AR model runs
    //                  unconditioned and we can scan the heatmap for any
    //                  leftover peak.
    //
    // Frames arrive in RGB order and the training script matches this
    // (cv2.imread + cv2.cvtColor(BGR2RGB)), so we pack in src order with no
    // swap. blobFromImage is SIMD-vectorized; memcpy all three planes at once.
    float* dartIn = m_impl->dart->input(0);
    for(uint32_t cam = 0; cam < N_CAMS; cam++)
    {
        const cv::Mat& w = m_impl->warped[cam];
        float* dst = dartIn + 3u * cam * PLANE_FLOATS;

        if(w.empty())
        {
            std::memset(dst, 0, 3u * PLANE_FLOATS * sizeof(float));
            continue;
        }

        cv::dnn::blobFromImage(w, m_impl->arBlob[cam],
                               /*scalefactor*/ 1.0 / 255.0,
                               /*size*/        cv::Size(),
                               /*mean*/        cv::Scalar(),
                               /*swapRB*/      false,
                               /*crop*/        false,
                               CV_32F);
        std::memcpy(dst, m_impl->arBlob[cam].ptr<float>(), 3u * PLANE_FLOATS * sizeof(float));
    }

    float* condBase = dartIn + COND_MASK_BASE * PLANE_FLOATS;
    std::memset(condBase, 0, COND_CHANNELS * PLANE_FLOATS * sizeof(float));

    // A camera with no frame this cycle, or a dart the segmenter could not
    // separate, leaves its channel zero — but still occupies its slot. Skipping
    // the slot instead would renumber every later dart and silently change what
    // the model is being told.
    if(m_impl->mode == Impl::Mode::Detecting)
    {
        const size_t slots = std::min<size_t>(m_impl->confirmedDarts.size(), MAX_COND_DARTS);
        for(size_t slot = 0; slot < slots; slot++)
        {
            const Impl::ConfirmedDart& d = m_impl->confirmedDarts[slot];

            for(uint32_t cam = 0; cam < N_CAMS; cam++)
            {
                if(d.instance[cam].empty()) continue;
                packMaskPlane(d.instance[cam],
                              dartIn + (COND_MASK_BASE + cam * MAX_COND_DARTS + slot)
                                     * PLANE_FLOATS);
            }

            addGaussianToPlane(dartIn + (COND_TIP_BASE + slot) * PLANE_FLOATS,
                               d.polar.templateX, d.polar.templateY);
        }
    }

    // ----- Inference ------------------------------------------------------
    // Submit the heavy U-Net first, then prepare and submit the palm stage, so
    // a backend with independent streams overlaps the two.
    if(!m_impl->dart->submit())
    {
        LOG_WARNING(DETECT_LOG_ID, "dart inference submit failed");
        return false;
    }

    const bool handPresent = m_impl->runPalmStage(rawFrames);

    if(!m_impl->dart->wait())
    {
        LOG_WARNING(DETECT_LOG_ID, "dart inference wait failed");
        return false;
    }

    m_impl->decodeAndTrack(handPresent, out);
    out.boardClear = m_impl->boardClear;
    out.status     = m_impl->formatStatus();
    return true;
}


// ----------------------------------------------------------------------------
// Hand presence: BlazePalm round-robin + landmark false-positive filter
// ----------------------------------------------------------------------------

bool DartDetector::Impl::runPalmStage(const std::array<cv::Mat, N_CAMS>& rawFrames)
{
    if(!config.enableHandFilter || !palm)
    {
        lastHandPresent = false;
        return false;
    }

    // One camera per cycle. We use the *raw* (unwarped) frame — the
    // perspective warp used for dart detection flattens the board plane and
    // shears anything in front of it, which makes hands unrecognizable to a
    // palm detector trained on natural photos.
    const uint32_t palmCam = palmFrameCounter++ % N_CAMS;
    bool palmDetectedThisCycle = false;

    if(!rawFrames[palmCam].empty())
    {
        cv::resize(rawFrames[palmCam], palmResized,
                   cv::Size(static_cast<int>(PALM_INPUT_W), static_cast<int>(PALM_INPUT_H)),
                   0, 0, cv::INTER_LINEAR);

        // Value range is [0, 1] for *full* BlazePalm. The lite model wanted
        // [-1, 1] but the full export's training contract differs — feeding
        // [-1, 1] produced saturated logits (sigmoid ~= 1.0) on every frame
        // regardless of image content. Verified by running the ONNX in
        // onnxruntime with both ranges on real captures.
        packNhwcNormalized(palmResized, palm->input(0), PALM_INPUT_W, PALM_INPUT_H);

        if(palm->run())
        {
            const float* scores = palm->output(0);
            const float* boxes  = palm->output(1);

            // Track the top-3 logits so we can tell saturation cases apart in
            // the diagnostic badge. A real hand fires a cluster of high
            // anchors; an isolated false positive is one big logit with the
            // rest near -inf.
            float t1 = -1e30f, t2 = -1e30f, t3 = -1e30f;
            uint32_t topIdx = 0;
            for(uint32_t i = 0; i < PALM_SCORES_FLOATS; i++)
            {
                const float v = scores[i];
                if(v > t1)      { t3 = t2; t2 = t1; t1 = v; topIdx = i; }
                else if(v > t2) { t3 = t2; t2 = v; }
                else if(v > t3) { t3 = v; }
            }
            palmTop1 = t1; palmTop2 = t2; palmTop3 = t3;

            const float bestProb = 1.0f / (1.0f + std::exp(-t1));
            lastPalmScore = bestProb;
            const bool palmStagePass = (bestProb >= PALM_PRESENCE_THRESHOLD);

            // ----- Stage 2: hand-landmark FP filter -----
            // Only run when palm already passed — saves the entire landmark
            // cost in the common "no hand in view" case. The result ANDs with
            // the palm result, so a real hand requires both a palm spike AND a
            // plausible 21-keypoint skeleton fit.
            bool  landmarkStagePass = false;
            float landmarkPresence  = -1.0f;
            if(palmStagePass && landmark)
            {
                // Decode the winning anchor's 18 regressor floats. Values are
                // in 192-pixel input space, anchor-center-relative for centers
                // and keypoints, absolute pixels for box w/h.
                const float* reg = boxes + static_cast<size_t>(topIdx) * PALM_BOX_DIM;
                const float  acx = kBlazePalmAnchorsCxCy[2 * topIdx + 0];
                const float  acy = kBlazePalmAnchorsCxCy[2 * topIdx + 1];

                const float bw = reg[2];
                const float bh = reg[3];

                const float kp0x = reg[4 + 2 * PALM_KP_WRIST  + 0] + acx;
                const float kp0y = reg[4 + 2 * PALM_KP_WRIST  + 1] + acy;
                const float kp2x = reg[4 + 2 * PALM_KP_MIDDLE + 0] + acx;
                const float kp2y = reg[4 + 2 * PALM_KP_MIDDLE + 1] + acy;
                const float bcx  = reg[0] + acx;
                const float bcy  = reg[1] + acy;

                // Map 192-space coords to raw-frame pixels.
                const int   rawW = rawFrames[palmCam].cols;
                const int   rawH = rawFrames[palmCam].rows;
                const float sx = static_cast<float>(rawW) / static_cast<float>(PALM_INPUT_W);
                const float sy = static_cast<float>(rawH) / static_cast<float>(PALM_INPUT_H);

                const float cxR  = bcx  * sx;
                const float cyR  = bcy  * sy;
                const float wR   = bw   * sx;
                const float hR   = bh   * sy;
                const float k0xR = kp0x * sx;
                const float k0yR = kp0y * sy;
                const float k2xR = kp2x * sx;
                const float k2yR = kp2y * sy;

                // Wrist→middle-finger MCP angle. Target is to point the
                // fingers up in the destination crop, i.e. rotate by
                // (-pi/2 - angle).
                const float angle = std::atan2(k2yR - k0yR, k2xR - k0xR);
                const float rot   = -static_cast<float>(M_PI) / 2.0f - angle;

                // Shift the crop center along the wrist→middle axis by
                // SHIFT_Y * longSide, mirroring MediaPipe's detection-to-roi
                // config (centers the box on the palm proper, not the wrist).
                const float longSide = std::max(wR, hR);
                const float side     = longSide * LANDMARK_CROP_EXPANSION;
                const float perpAng  = angle + static_cast<float>(M_PI) / 2.0f;
                const float cxS = cxR + LANDMARK_CROP_SHIFT_Y * longSide * std::cos(perpAng);
                const float cyS = cyR + LANDMARK_CROP_SHIFT_Y * longSide * std::sin(perpAng);

                if(side > 1.0f)
                {
                    // Build the 2x3 affine mapping src→dst:
                    //   dst = R(rot) * (src - center) * (target/side) + target/2
                    const float cosR  = std::cos(rot);
                    const float sinR  = std::sin(rot);
                    const float scale = static_cast<float>(LANDMARK_INPUT_W) / side;
                    const float halfT = static_cast<float>(LANDMARK_INPUT_W) / 2.0f;

                    cv::Mat M(2, 3, CV_32F);
                    M.at<float>(0, 0) =  cosR * scale;
                    M.at<float>(0, 1) = -sinR * scale;
                    M.at<float>(0, 2) =  halfT - scale * (cosR * cxS - sinR * cyS);
                    M.at<float>(1, 0) =  sinR * scale;
                    M.at<float>(1, 1) =  cosR * scale;
                    M.at<float>(1, 2) =  halfT - scale * (sinR * cxS + cosR * cyS);

                    cv::warpAffine(rawFrames[palmCam], landmarkCropRgb, M,
                                   cv::Size(static_cast<int>(LANDMARK_INPUT_W),
                                            static_cast<int>(LANDMARK_INPUT_H)),
                                   cv::INTER_LINEAR, cv::BORDER_CONSTANT,
                                   cv::Scalar(0, 0, 0));

                    packNhwcNormalized(landmarkCropRgb, landmark->input(0),
                                       LANDMARK_INPUT_W, LANDMARK_INPUT_H);

                    if(landmark->run())
                    {
                        // The model applies sigmoid internally, so the output
                        // is already in [0, 1] — no transform.
                        landmarkPresence  = landmark->output(1)[0];
                        landmarkStagePass = (landmarkPresence >= LANDMARK_PRESENCE_THRESHOLD);
                    }
                    else
                    {
                        LOG_WARNING(DETECT_LOG_ID, "landmark inference failed");
                    }
                }
            }
            lastLandmarkPresence = landmarkPresence;

            // Final gate: both stages must pass.
            palmDetectedThisCycle = palmStagePass && landmarkStagePass;
        }
        else
        {
            LOG_WARNING(DETECT_LOG_ID, "palm inference failed");
        }
    }

    palmRecent[palmCam] = palmDetectedThisCycle;
    bool handPresent = false;
    for(uint32_t i = 0; i < N_CAMS; i++)
    {
        if(palmRecent[i]) { handPresent = true; break; }
    }
    lastHandPresent = handPresent;
    return handPresent;
}


// ----------------------------------------------------------------------------
// Output decode + streak tracking
// ----------------------------------------------------------------------------

/**
 * Take each camera's share of the dart just confirmed, and fold it into the
 * running union.
 *
 * The segmenter emits one binary mask of every dart present, so a single
 * dart's pixels are only ever recoverable as a difference of successive
 * cumulative masks:
 *
 *     instance_j = M_j  &  ~M_(j-1)
 *
 * with M_j captured the instant dart j was counted. That is exactly the
 * derivation DartModelTraining's build_conditioning reproduces when it makes
 * the training labels, so the model has seen this and its artefacts: a thin rim
 * around a dart that has not moved (the board drifts between captures) and a
 * hole where a later dart passes behind an earlier one. Neither is worth
 * cleaning up — the model was trained with them.
 *
 * Called only on the cycle a dart is confirmed, which is why the warp lives
 * here rather than in the per-cycle path: it is three 720x720 remaps, and at
 * most three darts a turn need them.
 */
/**
 * Forget every counted dart, including the pixels they claimed.
 *
 * The union is the running M_(j-1); leaving it behind would have the first dart
 * of the next turn subtract against the last turn's board and come back empty.
 */
void DartDetector::Impl::clearConditioning()
{
    confirmedDarts.clear();
    for(uint32_t cam = 0; cam < N_CAMS; cam++) condUnion[cam].release();
}


void DartDetector::Impl::captureInstanceMasks(ConfirmedDart& dart)
{
    cv::Mat notPrev;
    cv::Mat claim;      // what this dart takes out of the mask before differencing

    for(uint32_t cam = 0; cam < N_CAMS; cam++)
    {
        // maskNative is this cycle's thresholded seg output, still live from
        // the masking step in run(). No frame, no calibration, no mask — the
        // slot stays empty and contributes a zeroed channel.
        if(maskNative[cam].empty()) continue;
        if(!warpCameraMask(cam, maskNative[cam], maskWarped[cam])
        || maskWarped[cam].empty())
        {
            continue;
        }

        if(config.stillFrameConditioning)
        {
            // The tip is in canonical coordinates and so is the warped mask, so
            // the same point locates the dart in every camera.
            if(!componentUnderTip(maskWarped[cam], dart.polar.templateX,
                                  dart.polar.templateY, claim))
            {
                continue;
            }
        }
        else
        {
            claim = maskWarped[cam];
        }

        if(condUnion[cam].empty())
        {
            condUnion[cam] = cv::Mat::zeros(maskWarped[cam].size(), CV_8UC1);
        }

        cv::bitwise_not(condUnion[cam], notPrev);
        cv::bitwise_and(claim, notPrev, dart.instance[cam]);
        cv::bitwise_or(condUnion[cam], claim, condUnion[cam]);
    }
}


void DartDetector::Impl::decodeAndTrack(bool handPresent, DartDetectorResult& out)
{
    const float* heatmapLogits = dart->output(0);
    const float  existLogit    = dart->output(1)[0];

    // ----- Find argmax of heatmap logits (matches sigmoid argmax) -----
    size_t bestIdx   = 0;
    float  bestLogit = heatmapLogits[0];
    for(size_t i = 1; i < HEATMAP_FLOATS; i++)
    {
        if(heatmapLogits[i] > bestLogit)
        {
            bestLogit = heatmapLogits[i];
            bestIdx   = i;
        }
    }

    // ----- Publish a sigmoid snapshot for the debug UI -----
    {
        std::vector<float> sigmoided(HEATMAP_FLOATS);
        for(size_t i = 0; i < HEATMAP_FLOATS; i++)
        {
            sigmoided[i] = 1.0f / (1.0f + std::exp(-heatmapLogits[i]));
        }
        std::lock_guard<std::mutex> lock(heatmapMutex);
        latestHeatmap  = std::move(sigmoided);
        latestHeatmapW = OUTPUT_W;
        latestHeatmapH = OUTPUT_H;
    }

    // ----- Gate on exist_logit and sigmoid(peak) -----
    const float bestProb = 1.0f / (1.0f + std::exp(-bestLogit));
    lastHeatmapPeak = bestProb;
    lastExistLogit  = existLogit;
    const bool hasDetection = (existLogit >= EXIST_THRESHOLD)
                           && (bestProb   >= HEATMAP_THRESHOLD);

    // The board-clear check used by Removing needs only the sigmoid threshold
    // — exist_logit and the catch-ring radius gate are skipped so a leftover
    // dart anywhere on the heatmap (even just outside the wire) blocks the
    // cleared signal.
    const bool anyPeakAboveThreshold = (bestProb >= HEATMAP_THRESHOLD);
    lastPeakAboveThresh = anyPeakAboveThreshold;

    // ----- State machine: Detecting <-> Removing -----
    // boardClear is driven exclusively from here. While Removing it stays
    // false; when the board has been quiet for clearHoldMs we flip back to
    // Detecting and set it true.
    const auto now = Clock::now();
    const bool busy = handPresent || anyPeakAboveThreshold;
    if(busy) lastBusy = now;

    const auto quietMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - lastBusy).count();

    if(mode == Mode::Removing)
    {
        // One condition, and it is a duration: the board must have been
        // continuously quiet — no hand, no leftover peak — for clearHoldMs.
        // Anything that looks busy restarts the clock, so a hand that pauses
        // mid-withdrawal does not let the turn end underneath it.
        if(quietMs >= clearHoldMs.load(std::memory_order_relaxed))
        {
            mode           = Mode::Detecting;
            handWasPresent = false;
            boardClear     = true;
            // Leaving Removing is what raises the board-clear edge the game
            // ends a turn on, so both transitions are logged: a spurious pair
            // is indistinguishable from a real collection at the score line,
            // but obvious here.
            LOG_INFO(DETECT_LOG_ID, "Removing -> Detecting (quiet for {} ms)", quietMs);
        }
        // No candidate tracking, no dart emission while Removing.
        return;
    }

    // ----- Detecting: check for hand entry first -----
    if(handPresent)
    {
        if(!handWasPresent)
        {
            handWasPresent = true;
            handSince      = now;
        }
        const auto handMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - handSince).count();
        if(handMs >= handEnterMs.load(std::memory_order_relaxed))
        {
            LOG_INFO(DETECT_LOG_ID,
                     "Detecting -> Removing (hand for {} ms, palm={:.2f}, "
                     "landmark={:.2f}, dropping {} confirmed dart(s))",
                     handMs, lastPalmScore, lastLandmarkPresence,
                     confirmedDarts.size());
            // Enter Removing: drop all dart state so the AR model runs
            // unconditioned next cycle and we can scan the heatmap for any
            // remaining dart. boardClear stays false until the clear streak
            // completes.
            mode = Mode::Removing;
            candidates.clear();
            clearConditioning();
            lastBusy   = now;   // the quiet clock starts when the hand leaves
            boardClear = false;
            return;
        }
    }
    else
    {
        handWasPresent = false;
    }

    // ----- Decode the peak -----
    // Plain cell centre: the new export has no offset head, matching
    // heatmap_utils.py:autoregressive_unroll, which takes (r + 0.5, c + 0.5).
    const uint32_t r = static_cast<uint32_t>(bestIdx / OUTPUT_W);
    const uint32_t c = static_cast<uint32_t>(bestIdx % OUTPUT_W);
    const float    peakR = static_cast<float>(r) + 0.5f;
    const float    peakC = static_cast<float>(c) + 0.5f;

    // ----- Convert heatmap-cell coords → polar (angle, normRadius) -----
    PolarDart detected{};
    bool      produced = false;
    if(hasDetection)
    {
        const float tx = peakC * HEATMAP_TO_TEMPLATE;
        const float ty = peakR * HEATMAP_TO_TEMPLATE;
        const float dx = tx - TEMPLATE_CENTER;
        const float dy = ty - TEMPLATE_CENTER;
        const float dist  = std::sqrt(dx * dx + dy * dy);
        const float normR = dist / BOARD_RADIUS_PX;

        if(normR <= MAX_DETECT_RADIUS)
        {
            detected.angle            = static_cast<float>(std::atan2(dy, dx) * 180.0 / M_PI);
            detected.normalizedRadius = normR;
            detected.templateX        = tx;
            detected.templateY        = ty;
            produced = true;
        }
    }

    // Board is clear iff no unmasked dart was detected AND no darts are
    // already confirmed. (The model masks confirmed darts, so it can't "see"
    // them — board-clear derives from what we know, not what it just
    // predicted.)
    boardClear = !produced && confirmedDarts.empty();

    // ----- Streak tracker — one detection per cycle simplifies matching -----
    const float matchR2 = DART_MATCH_RADIUS_PX * DART_MATCH_RADIUS_PX;
    auto distSqTemplate = [](const PolarDart& a, const PolarDart& b) -> float
    {
        const float ddx = a.templateX - b.templateX;
        const float ddy = a.templateY - b.templateY;
        return ddx * ddx + ddy * ddy;
    };

    bool      emittedThisCycle = false;
    PolarDart emitted{};

    if(produced)
    {
        int   bestCand = -1;
        float bestD2   = matchR2;
        for(size_t i = 0; i < candidates.size(); i++)
        {
            const float d2 = distSqTemplate(detected, candidates[i].polar);
            if(d2 < bestD2)
            {
                bestD2   = d2;
                bestCand = static_cast<int>(i);
            }
        }

        if(bestCand >= 0)
        {
            candidates[bestCand].polar = detected;
            candidates[bestCand].streak++;

            // Both gates: enough independent samples, and long enough in one
            // place that it cannot still be in the air.
            const auto heldMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - candidates[bestCand].firstSeen).count();
            if(candidates[bestCand].streak >= confirmFrames.load(std::memory_order_relaxed)
            && heldMs >= confirmHoldMs.load(std::memory_order_relaxed)
            && !candidates[bestCand].emitted)
            {
                candidates[bestCand].emitted = true;
                emitted = detected;
                emittedThisCycle = true;
            }
        }
        else
        {
            CandidateDart cd;
            cd.polar     = detected;
            cd.streak    = 1;
            cd.emitted   = false;
            cd.firstSeen = now;
            candidates.push_back(cd);
        }
    }

    // Drop candidates that didn't get a hit this cycle (either because the
    // detection was elsewhere, or because there was no detection at all).
    // With one detection per cycle, any candidate not matched above is gone.
    for(int i = static_cast<int>(candidates.size()) - 1; i >= 0; i--)
    {
        const bool matched = produced
                          && distSqTemplate(candidates[static_cast<size_t>(i)].polar, detected) < matchR2;
        if(!matched) candidates.erase(candidates.begin() + i);
    }

    if(emittedThisCycle)
    {
        // A dart was confirmed — add to the conditioning set so the next
        // inference sees its pixels and hunts for the next dart.
        ConfirmedDart cd;
        cd.polar = emitted;
        captureInstanceMasks(cd);
        confirmedDarts.push_back(std::move(cd));
        boardClear = false;
        out.newDarts.push_back({emitted.angle, emitted.normalizedRadius});
    }

    // Surface the largest candidate streak so the debug badge can show whether
    // we're accumulating frames toward a confirmation. If the heatmap is hot
    // but this stays stuck below confirmFrames, we know the streak isn't
    // completing (position wobble, transient gate failures) rather than the
    // gates being unreachable.
    int maxStreak = 0;
    for(const auto& cand : candidates)
    {
        if(cand.streak > maxStreak) maxStreak = cand.streak;
    }
    lastCandidateStreak = maxStreak;
}


// ----------------------------------------------------------------------------
// Diagnostic status string
// ----------------------------------------------------------------------------

std::string DartDetector::Impl::formatStatus() const
{
    auto fmt2 = [](float v) -> std::string {
        if(v < 0.0f) return "?";
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%.2f", v);
        return std::string(buf);
    };
    auto fmtLogit = [](float v) -> std::string {
        if(v <= -1e29f) return "?";
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%.1f", v);
        return std::string(buf);
    };
    auto fmtExist = [](float v) -> std::string {
        if(v <= -1e29f) return "?";
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%+.2f", v);
        return std::string(buf);
    };

    // Always-on tail showing live max-anchor palm sigmoid + best heatmap
    // sigmoid, plus top-3 raw palm logits. Watch these with hand/dart in and
    // out of frame to pick PALM_PRESENCE_THRESHOLD and HEATMAP_THRESHOLD
    // empirically — there should be a clear gap between the hand-out value and
    // the hand-in value. The top-3 logits help distinguish single-anchor noise
    // spikes (one big, rest tiny/negative) from a model that's confidently
    // producing nonsense (top-3 all saturated).
    const std::string scores
        = "  palm="  + fmt2(lastPalmScore)
        + " hm="     + fmt2(lastHeatmapPeak)
        + " exist="  + fmtExist(lastExistLogit)
        + " streak=" + std::to_string(lastCandidateStreak)
        + "/"        + std::to_string(confirmFrames.load(std::memory_order_relaxed))
        + " logits=" + fmtLogit(palmTop1)
        + "/"        + fmtLogit(palmTop2)
        + "/"        + fmtLogit(palmTop3);

    if(mode == Mode::Removing)
    {
        // Show which gate is blocking the clear streak. If we're stuck at
        // clear=0/N, hand=Y or peak=Y tells you which signal needs to go quiet
        // for the system to escape Removing.
        std::string flags;
        flags += " hand="; flags += (lastHandPresent     ? "Y" : "N");
        flags += " peak="; flags += (lastPeakAboveThresh ? "Y" : "N");
        // Countdown in seconds — a cycle count meant nothing once the rate
        // stopped being fixed, and this is the number a player is waiting on.
        const auto quietMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            Clock::now() - lastBusy).count();
        const long remaining = clearHoldMs.load(std::memory_order_relaxed)
                             - static_cast<long>(quietMs);
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.1fs", std::max(0L, remaining) / 1000.0);
        return std::string("Removing (clear in ") + buf + ")" + flags + scores;
    }

    if(handWasPresent)
    {
        const auto handMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            Clock::now() - handSince).count();
        return "Detecting (hand " + std::to_string(handMs) + "/"
             + std::to_string(handEnterMs.load(std::memory_order_relaxed)) + " ms)" + scores;
    }
    return "Detecting" + scores;
}
