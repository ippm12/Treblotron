/**
 * tensorrt_vision_source.cpp
 *
 * TensorRT-backed dart detection for Jetson. Grabs the latest pre-warped
 * frame from each camera every inference, packs them into a 10-channel
 * float NCHW tensor (3 cameras × 3 RGB + 1 conditioning mask), runs the
 * autoregressive multicam_unet_ar model via TensorRT FP16, and decodes the
 * (heatmap, offset, exist_logit) outputs into a single dart event per
 * inference.
 *
 * This file is only compiled when DARTLENS_USE_TENSORRT is enabled; the
 * Windows sim and Raspberry Pi (Hailo) builds skip it entirely so TensorRT
 * / CUDA headers only need to be available on the Jetson.
 */

#include "tensorrt_vision_source.hpp"
#include "vision/vision.hpp"
#include "vision/wire_calibration.hpp"
#include "dart/dart_board_geometry.hpp"

#include <NvInfer.h>
#include <NvOnnxParser.h>
#include <cuda_runtime.h>

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>


// ============================================================================
// Constants
// ============================================================================

namespace
{
    // Paths relative to the executable — the ONNX ships alongside the cached
    // .trt engine. Engine is rebuilt automatically when the ONNX is newer.
    // Cache path includes "_fp32exist" so any old engine built without the
    // exist-head precision pin (which produced NaN for the exist_logit due
    // to FP16 overflow in GlobalAveragePool) is ignored.
    constexpr const char* ONNX_PATH   = "./vision/multicam_unet_ar/multicam_unet_ar.onnx";
    constexpr const char* ENGINE_PATH = "./vision/multicam_unet_ar/multicam_unet_ar_fp32exist.trt";

    // Tensor names — must match the ONNX export in
    // DartModelTraining/export.py:190-194.
    constexpr const char* INPUT_NAME   = "input";
    constexpr const char* OUT_HEATMAP  = "heatmap";
    constexpr const char* OUT_OFFSET   = "offset";
    constexpr const char* OUT_EXIST    = "exist_logit";

    // Model IO shapes (must match the ONNX export).
    constexpr uint32_t INPUT_W   = 720;
    constexpr uint32_t INPUT_H   = 720;
    constexpr uint32_t INPUT_C   = 10;  // 3 cams × RGB + 1 conditioning mask
    constexpr uint32_t OUTPUT_W  = 360;
    constexpr uint32_t OUTPUT_H  = 360;
    constexpr uint32_t N_CAMS    = 3;

    // Decode thresholds — match DartModelTraining/heatmap_utils.py:376.
    constexpr float EXIST_THRESHOLD     = 0.0f;   // logit gate
    constexpr float HEATMAP_THRESHOLD   = 0.55f;  // sigmoid prob floor

    // Gaussian sigma for the conditioning mask — matches multicam_dataset.py.
    constexpr float MASK_SIGMA         = 5.0f;
    constexpr int   MASK_RADIUS_PX     = 20;  // ~4σ — per heatmap_dataset.py

    // A detection within this distance (template-space px) of a running
    // candidate is treated as the same dart.
    constexpr float DART_MATCH_RADIUS_PX = 16.0f;

    // Template-space geometry — shared with wire_calibration.cpp.
    constexpr float TEMPLATE_CENTER = 360.0f;
    constexpr float BOARD_RADIUS_PX = 290.0f;

    // Heatmap → template scale (720 / 360 = 2).
    constexpr float HEATMAP_TO_TEMPLATE = static_cast<float>(INPUT_W) / OUTPUT_W;

    // Accept peaks out to the catch-ring so misses just outside the double
    // wire can still be recorded. Matches the Hailo source.
    constexpr float MAX_DETECT_RADIUS = 1.35f;

    // Tensor byte sizes.
    constexpr size_t INPUT_FLOATS    = static_cast<size_t>(INPUT_C) * INPUT_H * INPUT_W;
    constexpr size_t HEATMAP_FLOATS  = static_cast<size_t>(OUTPUT_H) * OUTPUT_W;
    constexpr size_t OFFSET_FLOATS   = 2u * OUTPUT_H * OUTPUT_W;

    // Plane strides inside the NCHW input tensor (one plane = 720×720 floats).
    constexpr size_t PLANE_FLOATS    = static_cast<size_t>(INPUT_H) * INPUT_W;

    // Single source of truth for the FP16 workspace cap. 2 GiB is comfortably
    // above what this network needs and well under the Orin Nano Super's
    // 8 GB unified memory budget.
    constexpr size_t TRT_WORKSPACE_BYTES = 1ULL << 31;

    // ----- BlazePalm (presence-only hand detector) -----
    //
    // Round-robin one camera per cycle. Per-camera last-result memory is
    // OR'd to decide "hand present" so a held dart occluding one or two
    // camera angles doesn't keep us out of the Removing state. Output
    // tensor names + shapes match the standard BlazePalm Lite ONNX export
    // (see vision/palm_detection/README.md for the conversion recipe).
    constexpr const char* PALM_ONNX_PATH   = "./vision/palm_detection/blazepalm.onnx";
    // FP32 cache path: the full BlazePalm overflows FP16 and produces NaN, so
    // the palm engine has to be built in FP32. The new path makes any stale
    // FP16 cache from a prior build irrelevant — it'll just be ignored.
    constexpr const char* PALM_ENGINE_PATH = "./vision/palm_detection/blazepalm_fp32.trt";

    // Canonical I/O names. tf2onnx and most community converters spit out
    // generic names ("input_1" / "Identity" / "Identity_1") that depend on
    // TFLite output ordering and shift between exports. We rewrite them
    // to these stable names with vision/palm_detection/rename_io.py — run
    // it once after each fresh ONNX export. The runtime asserts on
    // mismatch (see setTensorAddress checks below) so a forgotten rename
    // can never silent-fail.
    constexpr const char* PALM_INPUT_NAME   = "input";
    constexpr const char* PALM_OUT_SCORES   = "classificators";
    constexpr const char* PALM_OUT_BOXES    = "regressors";

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
    // produces enough single-anchor noise spikes to keep the system stuck
    // in Removing (false positives reset the 10-frame clear streak).
    // 0.7 filters those without losing recall on real hands, which
    // typically score 0.9+. If field testing shows real-hand misses,
    // either lower this back to 0.6, or switch the aggregation from
    // max-of-anchors to top-K mean (more robust to single-anchor noise).
    constexpr float PALM_PRESENCE_THRESHOLD = 0.7f;
}


// ============================================================================
// TRT logger — forwards everything above INFO through the project logger.
// ============================================================================

namespace
{
    class TrtLogger : public nvinfer1::ILogger
    {
        public:
            void log(Severity sev, const char* msg) noexcept override
            {
                switch(sev)
                {
                    case Severity::kINTERNAL_ERROR:
                    case Severity::kERROR:
                        LOG_ERROR(VISION_LOG_ID, "TRT: {}", msg);
                        break;
                    case Severity::kWARNING:
                        LOG_WARNING(VISION_LOG_ID, "TRT: {}", msg);
                        break;
                    case Severity::kINFO:
                        LOG_INFO(VISION_LOG_ID, "TRT: {}", msg);
                        break;
                    case Severity::kVERBOSE:
                        // Intentionally dropped — TRT is very chatty at verbose.
                        break;
                }
            }
    };

    TrtLogger g_trtLogger;


    // ----------------------------------------------------------------
    // TRT progress monitor — does NOT touch the loading-screen progress
    // bar (that's driven by C++ phase markers in buildThreadMain so it
    // stays monotonic 0→100% across the whole startup, instead of
    // bouncing 0→100% multiple times as TRT cycles through internal
    // top-level phases). The monitor's only job is to bump a global
    // iteration counter on every step so the UI can show that the build
    // is still doing work during long-running phases like timing tactics.
    // ----------------------------------------------------------------
    class TrtProgressMonitor : public nvinfer1::IProgressMonitor
    {
        public:
            TrtProgressMonitor(std::atomic<uint64_t>& iterationOut,
                               std::atomic<bool>& abortFlag)
                : m_iteration(iterationOut)
                , m_abort(abortFlag)
            {}

            void phaseStart(char const* /*phaseName*/,
                            char const* /*parentPhase*/,
                            int32_t /*nbSteps*/) noexcept override {}

            bool stepComplete(char const* /*phaseName*/, int32_t /*step*/) noexcept override
            {
                m_iteration.fetch_add(1, std::memory_order_relaxed);
                return !m_abort.load(std::memory_order_acquire);
            }

            void phaseFinish(char const* /*phaseName*/) noexcept override {}

        private:
            std::atomic<uint64_t>& m_iteration;
            std::atomic<bool>&     m_abort;
    };


    // Returns true iff the engine exposes an I/O tensor with the given name.
    // Used at engine load time to assert the ONNX has the canonical names
    // the runtime hardcodes — setTensorAddress silently no-ops on unknown
    // names, so we have to explicitly check first.
    bool engineHasIOTensor(nvinfer1::ICudaEngine* engine, const char* name)
    {
        if(!engine || !name) return false;
        const int32_t n = engine->getNbIOTensors();
        for(int32_t i = 0; i < n; i++)
        {
            const char* t = engine->getIOTensorName(i);
            if(t && std::strcmp(t, name) == 0) return true;
        }
        return false;
    }


    // Small RAII helper for CUDA device allocations. Doesn't try to match
    // the full CUDA allocator API — we only need malloc + free here.
    struct CudaBuffer
    {
        void*  ptr   = nullptr;
        size_t bytes = 0;

        ~CudaBuffer() { free(); }

        CudaBuffer() = default;
        CudaBuffer(const CudaBuffer&) = delete;
        CudaBuffer& operator=(const CudaBuffer&) = delete;

        bool allocate(size_t n)
        {
            free();
            if(cudaMalloc(&ptr, n) != cudaSuccess)
            {
                ptr = nullptr;
                return false;
            }
            bytes = n;
            return true;
        }

        void free()
        {
            if(ptr) { cudaFree(ptr); ptr = nullptr; }
            bytes = 0;
        }
    };


    // Pinned-host staging buffer — enables true async H2D/D2H copies.
    struct PinnedBuffer
    {
        void*  ptr   = nullptr;
        size_t bytes = 0;

        ~PinnedBuffer() { free(); }

        PinnedBuffer() = default;
        PinnedBuffer(const PinnedBuffer&) = delete;
        PinnedBuffer& operator=(const PinnedBuffer&) = delete;

        bool allocate(size_t n)
        {
            free();
            if(cudaHostAlloc(&ptr, n, cudaHostAllocDefault) != cudaSuccess)
            {
                ptr = nullptr;
                return false;
            }
            bytes = n;
            return true;
        }

        void free()
        {
            if(ptr) { cudaFreeHost(ptr); ptr = nullptr; }
            bytes = 0;
        }
    };
}


// ============================================================================
// pImpl — all TensorRT / CUDA handles live here so the header stays clean.
// ============================================================================

struct TensorRTVisionSource::Impl
{
    std::unique_ptr<nvinfer1::IRuntime>          runtime;
    std::unique_ptr<nvinfer1::ICudaEngine>       engine;
    std::unique_ptr<nvinfer1::IExecutionContext> context;

    cudaStream_t stream = nullptr;

    // Device buffers (persistent across inferences).
    CudaBuffer dInput;     // INPUT_FLOATS    * sizeof(float)
    CudaBuffer dHeatmap;   // HEATMAP_FLOATS  * sizeof(float)
    CudaBuffer dOffset;    // OFFSET_FLOATS   * sizeof(float)
    CudaBuffer dExist;     // 1               * sizeof(float)

    // Pinned host staging — input is written from the CPU each frame, then
    // cudaMemcpyAsync'd to dInput. Outputs come back into pinned memory too.
    PinnedBuffer hInput;   // Aliased as float* below.
    PinnedBuffer hHeatmap;
    PinnedBuffer hOffset;
    PinnedBuffer hExist;

    // Warp destinations for each camera (720×720 BGR8, recycled each frame).
    cv::Mat warped[N_CAMS];

    // Latest camera frames copied out of the capture system.
    CameraFrame cameraFrames[N_CAMS];

    // ----- BlazePalm (second engine, separate stream so it can overlap) -----
    std::unique_ptr<nvinfer1::ICudaEngine>       palmEngine;
    std::unique_ptr<nvinfer1::IExecutionContext> palmContext;

    cudaStream_t palmStream = nullptr;

    CudaBuffer dPalmInput;    // PALM_INPUT_FLOATS  * sizeof(float)
    CudaBuffer dPalmScores;   // PALM_SCORES_FLOATS * sizeof(float)
    CudaBuffer dPalmBoxes;    // PALM_BOXES_FLOATS  * sizeof(float) — bound, never read

    PinnedBuffer hPalmInput;
    PinnedBuffer hPalmScores;

    // Raw frame copy + resize destination for the round-robin camera frame.
    // BlazePalm needs the *unwarped* image — the perspective warp used for
    // dart detection flattens the board plane and shears anything in front
    // of it, which makes hands unrecognizable to a palm detector trained on
    // natural photos.
    CameraFrame palmRawFrame;
    cv::Mat     palmResized;  // 192×192 RGB8
};


// ============================================================================
// Helpers
// ============================================================================

namespace
{
    bool readFileBytes(const std::string& path, std::vector<uint8_t>& out)
    {
        std::ifstream f(path, std::ios::binary);
        if(!f) return false;
        f.seekg(0, std::ios::end);
        const std::streampos sz = f.tellg();
        if(sz <= 0) return false;
        f.seekg(0, std::ios::beg);
        out.resize(static_cast<size_t>(sz));
        return static_cast<bool>(f.read(reinterpret_cast<char*>(out.data()), sz));
    }


    // Cache key: rebuild whenever the ONNX file is newer than the cached
    // engine. Deserialization failures (GPU/TRT version mismatch) also
    // trigger a rebuild further down.
    bool cachedEngineIsFresh(const std::string& enginePath, const std::string& onnxPath)
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        if(!fs::exists(enginePath, ec) || ec) return false;
        if(!fs::exists(onnxPath, ec) || ec) return false;
        auto engineMt = fs::last_write_time(enginePath, ec);
        if(ec) return false;
        auto onnxMt = fs::last_write_time(onnxPath, ec);
        if(ec) return false;
        return engineMt >= onnxMt;
    }


    // Build an engine from the ONNX and serialize it to disk. Logs the
    // (multi-second) build cost so the first-run delay doesn't look like a
    // hang. `useFp16` controls precision — networks with deep activation
    // chains (e.g. the full BlazePalm) overflow FP16's ±65504 range and
    // produce NaN outputs, so they need FP32. `fp32LayerNames` selectively
    // pins specific layers to FP32 (exact ONNX node-name match) while the
    // rest of the engine stays at the requested precision — used to fix
    // individual unstable layers (e.g. the dart model's GlobalAveragePool
    // that overflows when summing 129k FP16 activations into the
    // exist_logit head). Optionally accepts a progress monitor so the UI
    // can draw a progress bar during the build.
    std::vector<uint8_t> buildEngineFromOnnx(
        const std::string& onnxPath,
        nvinfer1::IProgressMonitor* monitor,
        bool useFp16 = true,
        const std::vector<std::string>& fp32LayerNames = {})
    {
        const char* precisionLabel = useFp16 ? "FP16" : "FP32";
        LOG_INFO(VISION_LOG_ID,
                 "TensorRTVisionSource: building {} engine from {} "
                 "(first run only, typically 2-5 minutes on Orin Nano Super)",
                 precisionLabel, onnxPath);

        std::unique_ptr<nvinfer1::IBuilder> builder(
            nvinfer1::createInferBuilder(g_trtLogger));
        if(!builder) { LOG_ERROR(VISION_LOG_ID, "createInferBuilder failed"); return {}; }

        // TRT 10 uses strongly-typed explicit-batch networks by default —
        // passing 0 flags is correct.
        std::unique_ptr<nvinfer1::INetworkDefinition> network(builder->createNetworkV2(0));
        if(!network) { LOG_ERROR(VISION_LOG_ID, "createNetworkV2 failed"); return {}; }

        std::unique_ptr<nvonnxparser::IParser> parser(
            nvonnxparser::createParser(*network, g_trtLogger));
        if(!parser) { LOG_ERROR(VISION_LOG_ID, "nvonnxparser::createParser failed"); return {}; }

        if(!parser->parseFromFile(onnxPath.c_str(),
                                  static_cast<int>(nvinfer1::ILogger::Severity::kWARNING)))
        {
            for(int i = 0; i < parser->getNbErrors(); i++)
            {
                LOG_ERROR(VISION_LOG_ID, "ONNX parse error: {}", parser->getError(i)->desc());
            }
            return {};
        }

        std::unique_ptr<nvinfer1::IBuilderConfig> config(builder->createBuilderConfig());
        if(!config) { LOG_ERROR(VISION_LOG_ID, "createBuilderConfig failed"); return {}; }

        if(useFp16)
        {
            if(builder->platformHasFastFp16())
            {
                config->setFlag(nvinfer1::BuilderFlag::kFP16);
            }
            else
            {
                LOG_WARNING(VISION_LOG_ID,
                            "TRT: platform reports no fast FP16 — falling back to FP32");
            }
        }

        // Selectively pin per-layer precision. With the OBEY flag set TRT is
        // required to honor the constraint or fail the build — the alternative
        // (PREFER) silently falls back to FP16, which would re-introduce the
        // overflow we're trying to escape. Names match the exact ONNX node
        // name (e.g. "/m/Squeeze") so we don't accidentally catch something
        // like "/m/Squeeze_1" — that bit us once already.
        if(!fp32LayerNames.empty())
        {
            config->setFlag(nvinfer1::BuilderFlag::kOBEY_PRECISION_CONSTRAINTS);

            const int32_t nbLayers = network->getNbLayers();
            std::vector<bool> matched(fp32LayerNames.size(), false);
            for(int32_t i = 0; i < nbLayers; i++)
            {
                nvinfer1::ILayer* layer = network->getLayer(i);
                if(!layer) continue;
                const char* lname = layer->getName();
                if(!lname) continue;

                int matchIdx = -1;
                for(size_t k = 0; k < fp32LayerNames.size(); k++)
                {
                    if(fp32LayerNames[k] == lname) { matchIdx = static_cast<int>(k); break; }
                }
                if(matchIdx < 0) continue;

                layer->setPrecision(nvinfer1::DataType::kFLOAT);
                const int32_t nbOuts = layer->getNbOutputs();
                for(int32_t o = 0; o < nbOuts; o++)
                {
                    layer->setOutputType(o, nvinfer1::DataType::kFLOAT);
                }
                LOG_INFO(VISION_LOG_ID, "TRT: pinned layer {} to FP32", lname);
                matched[matchIdx] = true;
            }

            for(size_t k = 0; k < fp32LayerNames.size(); k++)
            {
                if(!matched[k])
                {
                    LOG_WARNING(VISION_LOG_ID,
                                "TRT: requested FP32 pin for layer {} not found in network "
                                "— check the ONNX node names",
                                fp32LayerNames[k]);
                }
            }
        }

        config->setMemoryPoolLimit(nvinfer1::MemoryPoolType::kWORKSPACE, TRT_WORKSPACE_BYTES);
        if(monitor)
        {
            config->setProgressMonitor(monitor);
        }

        std::unique_ptr<nvinfer1::IHostMemory> plan(
            builder->buildSerializedNetwork(*network, *config));
        if(!plan)
        {
            LOG_ERROR(VISION_LOG_ID, "buildSerializedNetwork failed");
            return {};
        }

        std::vector<uint8_t> out(plan->size());
        std::memcpy(out.data(), plan->data(), plan->size());
        LOG_INFO(VISION_LOG_ID, "TensorRTVisionSource: engine built ({} bytes)", out.size());
        return out;
    }


    // Rasterize a Gaussian spot at (cx, cy) into a 720×720 float plane using
    // element-wise max — mirrors heatmap_dataset.py:generate_heatmap.
    void addGaussianToPlane(float* plane, float cx, float cy)
    {
        const int ix = static_cast<int>(std::lround(cx));
        const int iy = static_cast<int>(std::lround(cy));
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
                if(g > row[x]) row[x] = g;
            }
        }
    }
}


// ============================================================================
// Lifecycle
// ============================================================================

TensorRTVisionSource::TensorRTVisionSource() : m_impl(std::make_unique<Impl>())
{
}


TensorRTVisionSource::~TensorRTVisionSource() = default;


Status TensorRTVisionSource::init()
{
    try
    {
    // Start the camera system up front — it takes a moment too, and doing
    // it on the main thread keeps the threading model simple. The heavy
    // TRT engine build happens on a background thread below so the UI
    // stays interactive.
    initializeCameraSystem();

    LOG_INFO(VISION_LOG_ID, "TensorRTVisionSource: creating CUDA stream");
    if(cudaStreamCreate(&m_impl->stream) != cudaSuccess)
    {
        LOG_ERROR(VISION_LOG_ID, "cudaStreamCreate failed");
        m_buildState.store(BuildState::Failed, std::memory_order_release);
        return STATUS_ERROR_GENERIC;
    }

    {
        std::lock_guard<std::mutex> lock(m_buildStatusMutex);
        m_buildStatus = "Preparing";
    }
    m_buildProgress.store(0.0f, std::memory_order_release);
    m_buildState.store(BuildState::Building, std::memory_order_release);
    m_buildThread = std::thread(&TensorRTVisionSource::buildThreadMain, this);

    // The inference thread is spawned inside buildThreadMain() once the
    // engine is ready. init() returns immediately so the main render
    // loop can draw a loading screen.
    return STATUS_OK;
    }
    catch(const std::exception& e)
    {
        LOG_ERROR(VISION_LOG_ID, "TensorRTVisionSource::init threw: {}", e.what());
        m_buildState.store(BuildState::Failed, std::memory_order_release);
        return STATUS_ERROR_GENERIC;
    }
    catch(...)
    {
        LOG_ERROR(VISION_LOG_ID, "TensorRTVisionSource::init threw unknown exception");
        m_buildState.store(BuildState::Failed, std::memory_order_release);
        return STATUS_ERROR_GENERIC;
    }
}


// Runs on m_buildThread. Owns the full "bring the engine up" sequence:
// load cached engine (or build + cache it), create context, allocate
// buffers, bind tensors, and finally launch the inference thread.
//
// Progress reporting is monotonic across the whole startup — the bar
// fills 0 → 100% once total, advanced at the boundaries below. The
// raw TRT step counter is folded into m_buildIteration so the loading
// screen can show "still doing something" feedback during the long
// internal phases (timing tactics etc.) without bouncing the bar back
// to 0 each time TRT enters a new top-level phase.
void TensorRTVisionSource::buildThreadMain()
{
    auto setStatus = [&](const std::string& s)
    {
        std::lock_guard<std::mutex> lock(m_buildStatusMutex);
        m_buildStatus = s;
    };
    auto setPhase = [&](float pct, const std::string& s)
    {
        // Monotonic — never let a phase regress the bar.
        const float prev = m_buildProgress.load(std::memory_order_relaxed);
        if(pct > prev) m_buildProgress.store(pct, std::memory_order_release);
        setStatus(s);
    };
    auto fail = [&](const char* why)
    {
        LOG_ERROR(VISION_LOG_ID, "TensorRTVisionSource build failed: {}", why);
        setStatus(std::string("Failed: ") + why);
        m_buildState.store(BuildState::Failed, std::memory_order_release);
    };

    try
    {
        // ---- Dart engine: load from cache or build from ONNX ----------
        setPhase(0.05f, "Loading dart detector");
        std::vector<uint8_t> planBytes;
        bool loadedFromCache = false;
        if(cachedEngineIsFresh(ENGINE_PATH, ONNX_PATH)
        && readFileBytes(ENGINE_PATH, planBytes))
        {
            LOG_INFO(VISION_LOG_ID,
                     "TensorRTVisionSource: loaded cached dart engine from {} ({} bytes)",
                     ENGINE_PATH, planBytes.size());
            loadedFromCache = true;
        }

        m_impl->runtime.reset(nvinfer1::createInferRuntime(g_trtLogger));
        if(!m_impl->runtime) return fail("createInferRuntime");

        if(loadedFromCache)
        {
            m_impl->engine.reset(m_impl->runtime->deserializeCudaEngine(
                planBytes.data(), planBytes.size()));
            if(!m_impl->engine)
            {
                LOG_WARNING(VISION_LOG_ID,
                            "Cached dart engine failed to deserialize "
                            "(likely GPU/TRT version mismatch) — rebuilding");
                planBytes.clear();
                loadedFromCache = false;
            }
        }

        if(!loadedFromCache)
        {
            setPhase(0.05f, "Building dart detector");
            TrtProgressMonitor monitor(m_buildIteration, m_buildAbort);

            // Pin the exist_logit head's four exist-only layers to FP32. Their
            // GlobalAveragePool sums 360×360 = 129,600 FP16 activations into a
            // single scalar, which can overflow FP16's ±65504 range and
            // poison the rest of that head with Inf/NaN. The heatmap and
            // offset heads branch off earlier (at /m/up1/conv/conv.8/Relu)
            // and don't have a global-sum step, so they stay FP16.
            const std::vector<std::string> dartFp32Layers = {
                "/m/GlobalAveragePool",
                "/m/Flatten",
                "/m/exist_head/Gemm",
                "/m/Squeeze",
            };
            planBytes = buildEngineFromOnnx(ONNX_PATH, &monitor,
                                            /*useFp16*/ true,
                                            dartFp32Layers);
            if(planBytes.empty() || m_buildAbort.load(std::memory_order_acquire))
            {
                return fail("dart engine build");
            }

            setPhase(0.40f, "Caching dart detector to disk");
            std::ofstream f(ENGINE_PATH, std::ios::binary);
            if(f.write(reinterpret_cast<const char*>(planBytes.data()),
                       static_cast<std::streamsize>(planBytes.size())))
            {
                LOG_INFO(VISION_LOG_ID,
                         "TensorRTVisionSource: wrote dart engine cache to {}", ENGINE_PATH);
            }
            else
            {
                LOG_WARNING(VISION_LOG_ID,
                            "TensorRTVisionSource: failed to write dart engine cache to {} "
                            "(errno={}); will rebuild next run",
                            ENGINE_PATH, errno);
            }

            m_impl->engine.reset(m_impl->runtime->deserializeCudaEngine(
                planBytes.data(), planBytes.size()));
            if(!m_impl->engine) return fail("deserializeCudaEngine on fresh dart plan");
        }

        if(m_buildAbort.load(std::memory_order_acquire))
        {
            setStatus("Aborted");
            m_buildState.store(BuildState::Failed, std::memory_order_release);
            return;
        }

        // ---- Dart engine: validate I/O names + allocate + bind --------
        setPhase(0.42f, "Binding dart-detector tensors");
        if(!engineHasIOTensor(m_impl->engine.get(), INPUT_NAME))
            return fail("dart ONNX has no 'input' tensor — re-export with canonical names");
        if(!engineHasIOTensor(m_impl->engine.get(), OUT_HEATMAP))
            return fail("dart ONNX has no 'heatmap' tensor — re-export with canonical names");
        if(!engineHasIOTensor(m_impl->engine.get(), OUT_OFFSET))
            return fail("dart ONNX has no 'offset' tensor — re-export with canonical names");
        if(!engineHasIOTensor(m_impl->engine.get(), OUT_EXIST))
            return fail("dart ONNX has no 'exist_logit' tensor — re-export with canonical names");

        m_impl->context.reset(m_impl->engine->createExecutionContext());
        if(!m_impl->context) return fail("createExecutionContext (dart)");

        if(!m_impl->dInput.allocate(INPUT_FLOATS * sizeof(float))
        || !m_impl->dHeatmap.allocate(HEATMAP_FLOATS * sizeof(float))
        || !m_impl->dOffset.allocate(OFFSET_FLOATS * sizeof(float))
        || !m_impl->dExist.allocate(sizeof(float))
        || !m_impl->hInput.allocate(INPUT_FLOATS * sizeof(float))
        || !m_impl->hHeatmap.allocate(HEATMAP_FLOATS * sizeof(float))
        || !m_impl->hOffset.allocate(OFFSET_FLOATS * sizeof(float))
        || !m_impl->hExist.allocate(sizeof(float)))
        {
            return fail("CUDA buffer allocation (dart)");
        }

        if(!m_impl->context->setTensorAddress(INPUT_NAME,  m_impl->dInput.ptr))
            return fail("setTensorAddress (dart input)");
        if(!m_impl->context->setTensorAddress(OUT_HEATMAP, m_impl->dHeatmap.ptr))
            return fail("setTensorAddress (dart heatmap)");
        if(!m_impl->context->setTensorAddress(OUT_OFFSET,  m_impl->dOffset.ptr))
            return fail("setTensorAddress (dart offset)");
        if(!m_impl->context->setTensorAddress(OUT_EXIST,   m_impl->dExist.ptr))
            return fail("setTensorAddress (dart exist_logit)");

        if(m_buildAbort.load(std::memory_order_acquire))
        {
            setStatus("Aborted");
            m_buildState.store(BuildState::Failed, std::memory_order_release);
            return;
        }

        // ---- Palm engine: same load-or-build flow on a second engine -
        setPhase(0.50f, "Loading palm detector");
        std::vector<uint8_t> palmPlanBytes;
        bool palmFromCache = false;
        if(cachedEngineIsFresh(PALM_ENGINE_PATH, PALM_ONNX_PATH)
        && readFileBytes(PALM_ENGINE_PATH, palmPlanBytes))
        {
            LOG_INFO(VISION_LOG_ID,
                     "TensorRTVisionSource: loaded cached palm engine from {} ({} bytes)",
                     PALM_ENGINE_PATH, palmPlanBytes.size());
            palmFromCache = true;
        }

        if(palmFromCache)
        {
            m_impl->palmEngine.reset(m_impl->runtime->deserializeCudaEngine(
                palmPlanBytes.data(), palmPlanBytes.size()));
            if(!m_impl->palmEngine)
            {
                LOG_WARNING(VISION_LOG_ID,
                            "Cached palm engine failed to deserialize "
                            "(likely GPU/TRT version mismatch) — rebuilding");
                palmPlanBytes.clear();
                palmFromCache = false;
            }
        }

        if(!palmFromCache)
        {
            setPhase(0.50f, "Building palm detector");
            TrtProgressMonitor palmMonitor(m_buildIteration, m_buildAbort);
            // Force FP32 — full BlazePalm's deeper activation chain
            // overflows FP16, producing NaN scores. See buildEngineFromOnnx.
            palmPlanBytes = buildEngineFromOnnx(PALM_ONNX_PATH, &palmMonitor,
                                                /*useFp16*/ false);
            if(palmPlanBytes.empty() || m_buildAbort.load(std::memory_order_acquire))
            {
                return fail("palm engine build");
            }

            setPhase(0.85f, "Caching palm detector to disk");
            std::ofstream pf(PALM_ENGINE_PATH, std::ios::binary);
            if(pf.write(reinterpret_cast<const char*>(palmPlanBytes.data()),
                        static_cast<std::streamsize>(palmPlanBytes.size())))
            {
                LOG_INFO(VISION_LOG_ID,
                         "TensorRTVisionSource: wrote palm engine cache to {}",
                         PALM_ENGINE_PATH);
            }
            else
            {
                LOG_WARNING(VISION_LOG_ID,
                            "TensorRTVisionSource: failed to write palm engine "
                            "cache to {} (errno={}); will rebuild next run",
                            PALM_ENGINE_PATH, errno);
            }

            m_impl->palmEngine.reset(m_impl->runtime->deserializeCudaEngine(
                palmPlanBytes.data(), palmPlanBytes.size()));
            if(!m_impl->palmEngine) return fail("deserializeCudaEngine on fresh palm plan");
        }

        if(m_buildAbort.load(std::memory_order_acquire))
        {
            setStatus("Aborted");
            m_buildState.store(BuildState::Failed, std::memory_order_release);
            return;
        }

        // ---- Palm engine: validate I/O names + allocate + bind --------
        setPhase(0.90f, "Binding palm-detector tensors");

        // Dump the real names + shapes for diagnostic visibility — makes
        // future name mismatches obvious from the log.
        const int32_t nbPalmTensors = m_impl->palmEngine->getNbIOTensors();
        for(int32_t i = 0; i < nbPalmTensors; i++)
        {
            const char* name = m_impl->palmEngine->getIOTensorName(i);
            const auto  dims = m_impl->palmEngine->getTensorShape(name);
            std::string shape = "[";
            for(int32_t d = 0; d < dims.nbDims; d++)
            {
                if(d) shape += ",";
                shape += std::to_string(dims.d[d]);
            }
            shape += "]";
            LOG_INFO(VISION_LOG_ID, "palm engine tensor {}: {} {}", i, name, shape);
        }

        if(!engineHasIOTensor(m_impl->palmEngine.get(), PALM_INPUT_NAME))
            return fail("palm ONNX has no 'input' tensor — re-export with canonical names");
        if(!engineHasIOTensor(m_impl->palmEngine.get(), PALM_OUT_SCORES))
            return fail("palm ONNX has no 'classificators' tensor — re-export with canonical names");
        if(!engineHasIOTensor(m_impl->palmEngine.get(), PALM_OUT_BOXES))
            return fail("palm ONNX has no 'regressors' tensor — re-export with canonical names");

        m_impl->palmContext.reset(m_impl->palmEngine->createExecutionContext());
        if(!m_impl->palmContext) return fail("createExecutionContext (palm)");

        if(!m_impl->dPalmInput.allocate(PALM_INPUT_FLOATS  * sizeof(float))
        || !m_impl->dPalmScores.allocate(PALM_SCORES_FLOATS * sizeof(float))
        || !m_impl->dPalmBoxes.allocate(PALM_BOXES_FLOATS  * sizeof(float))
        || !m_impl->hPalmInput.allocate(PALM_INPUT_FLOATS  * sizeof(float))
        || !m_impl->hPalmScores.allocate(PALM_SCORES_FLOATS * sizeof(float)))
        {
            return fail("CUDA palm-detector buffer allocation");
        }

        if(!m_impl->palmContext->setTensorAddress(PALM_INPUT_NAME,  m_impl->dPalmInput.ptr))
            return fail("setTensorAddress (palm input)");
        if(!m_impl->palmContext->setTensorAddress(PALM_OUT_SCORES,  m_impl->dPalmScores.ptr))
            return fail("setTensorAddress (palm scores)");
        if(!m_impl->palmContext->setTensorAddress(PALM_OUT_BOXES,   m_impl->dPalmBoxes.ptr))
            return fail("setTensorAddress (palm boxes)");

        if(cudaStreamCreate(&m_impl->palmStream) != cudaSuccess)
        {
            return fail("cudaStreamCreate (palm)");
        }

        // ---- Launch the inference thread -----------------------------
        setPhase(0.98f, "Starting inference");
        m_running.store(true, std::memory_order_release);
        m_thread = std::thread(&TensorRTVisionSource::inferenceLoop, this);

        m_buildProgress.store(1.0f, std::memory_order_release);
        setStatus("Ready");
        m_buildState.store(BuildState::Ready, std::memory_order_release);
        LOG_INFO(VISION_LOG_ID, "TensorRTVisionSource initialized");
    }
    catch(const std::exception& e)
    {
        LOG_ERROR(VISION_LOG_ID, "buildThreadMain threw: {}", e.what());
        fail(e.what());
    }
    catch(...)
    {
        LOG_ERROR(VISION_LOG_ID, "buildThreadMain threw unknown exception");
        fail("unknown exception");
    }
}


bool TensorRTVisionSource::isInitializing() const
{
    return m_buildState.load(std::memory_order_acquire) == BuildState::Building;
}


bool TensorRTVisionSource::isFailed() const
{
    return m_buildState.load(std::memory_order_acquire) == BuildState::Failed;
}


float TensorRTVisionSource::getInitProgress() const
{
    return m_buildProgress.load(std::memory_order_acquire);
}


uint64_t TensorRTVisionSource::getInitIteration() const
{
    return m_buildIteration.load(std::memory_order_relaxed);
}


std::string TensorRTVisionSource::getInitStatus() const
{
    std::lock_guard<std::mutex> lock(m_buildStatusMutex);
    return m_buildStatus;
}


std::string TensorRTVisionSource::getDetectionStatus() const
{
    const DetectionMode mode = m_mode.load(std::memory_order_relaxed);
    const int handStreak     = m_handStreak.load(std::memory_order_relaxed);
    const int clearStreak    = m_clearStreak.load(std::memory_order_relaxed);
    const bool hand          = m_lastHandPresent.load(std::memory_order_relaxed);
    const bool peak          = m_lastPeakAboveThresh.load(std::memory_order_relaxed);
    const float palmScore    = m_lastPalmScore.load(std::memory_order_relaxed);
    const float heatmapPeak  = m_lastHeatmapPeak.load(std::memory_order_relaxed);

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

    // Always-on tail showing live max-anchor palm sigmoid + best
    // heatmap sigmoid, plus top-3 raw palm logits. Watch these with
    // hand/dart in and out of frame to pick PALM_PRESENCE_THRESHOLD
    // and HEATMAP_THRESHOLD empirically — there should be a clear
    // gap between the hand-out value and the hand-in value. The top-3
    // logits help distinguish single-anchor noise spikes (one big,
    // rest tiny/negative) from a model that's confidently producing
    // nonsense (top-3 all saturated).
    const float existLogit_ = m_lastExistLogit.load(std::memory_order_relaxed);
    const int   streakNow   = m_lastCandidateStreak.load(std::memory_order_relaxed);
    auto fmtExist = [](float v) -> std::string {
        if(v <= -1e29f) return "?";
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%+.2f", v);
        return std::string(buf);
    };

    const std::string scores
        = "  palm=" + fmt2(palmScore)
        + " hm="    + fmt2(heatmapPeak)
        + " exist=" + fmtExist(existLogit_)
        + " streak=" + std::to_string(streakNow) + "/" + std::to_string(CONFIRM_FRAMES)
        + " logits=" + fmtLogit(m_palmTop1.load(std::memory_order_relaxed))
        + "/"        + fmtLogit(m_palmTop2.load(std::memory_order_relaxed))
        + "/"        + fmtLogit(m_palmTop3.load(std::memory_order_relaxed));

    if(mode == DetectionMode::Removing)
    {
        // Show which gate is blocking the clear streak. If we're stuck
        // at clear=0/N, hand=Y or peak=Y tells you which signal needs
        // to go quiet for the system to escape Removing.
        std::string flags;
        flags += " hand=";  flags += (hand ? "Y" : "N");
        flags += " peak=";  flags += (peak ? "Y" : "N");
        return "Removing (clear " + std::to_string(clearStreak)
             + "/" + std::to_string(CLEAR_CONFIRM_FRAMES) + ")"
             + flags + scores;
    }

    if(handStreak > 0)
    {
        return "Detecting (entering " + std::to_string(handStreak)
             + "/" + std::to_string(HAND_ENTER_FRAMES) + ")" + scores;
    }
    return "Detecting" + scores;
}


void TensorRTVisionSource::shutdown()
{
    // If engine build is still running, signal abort via the progress
    // monitor's continueFlag and wait for the thread to unwind.
    m_buildAbort.store(true, std::memory_order_release);
    if(m_buildThread.joinable()) m_buildThread.join();
    // Build thread may have spawned the inference thread before seeing the
    // abort — explicitly stop it now.
    m_running.store(false, std::memory_order_release);
    if(m_thread.joinable())      m_thread.join();

    m_impl->palmContext.reset();
    m_impl->palmEngine.reset();
    m_impl->context.reset();
    m_impl->engine.reset();
    m_impl->runtime.reset();

    m_impl->dInput.free();
    m_impl->dHeatmap.free();
    m_impl->dOffset.free();
    m_impl->dExist.free();
    m_impl->hInput.free();
    m_impl->hHeatmap.free();
    m_impl->hOffset.free();
    m_impl->hExist.free();

    m_impl->dPalmInput.free();
    m_impl->dPalmScores.free();
    m_impl->dPalmBoxes.free();
    m_impl->hPalmInput.free();
    m_impl->hPalmScores.free();

    if(m_impl->stream)
    {
        cudaStreamDestroy(m_impl->stream);
        m_impl->stream = nullptr;
    }
    if(m_impl->palmStream)
    {
        cudaStreamDestroy(m_impl->palmStream);
        m_impl->palmStream = nullptr;
    }

    shutdownCameraSystem();
    LOG_INFO(VISION_LOG_ID, "TensorRTVisionSource shut down");
}


// ============================================================================
// Per-frame hook — drains events queued by the inference thread
// ============================================================================

void TensorRTVisionSource::tick(float deltaTime)
{
    (void)deltaTime;

    std::queue<std::pair<float, float>> drained;
    {
        std::lock_guard<std::mutex> lock(m_eventMutex);
        std::swap(drained, m_newDartEvents);
    }

    while(!drained.empty())
    {
        const auto& [angle, normR] = drained.front();
        if(m_onDartLanded) m_onDartLanded();
        if(m_onDartPositionCalculated) m_onDartPositionCalculated(angle, normR);
        drained.pop();
    }
}


bool TensorRTVisionSource::isBoardClear() const
{
    return m_boardClear.load(std::memory_order_acquire);
}


void TensorRTVisionSource::resetDarts()
{
    m_resetRequested.store(true, std::memory_order_release);
}


bool TensorRTVisionSource::getLatestHeatmap(std::vector<float>& out,
                                            uint32_t& width, uint32_t& height) const
{
    std::lock_guard<std::mutex> lock(m_heatmapMutex);
    if(m_latestHeatmap.empty()) return false;
    out    = m_latestHeatmap;
    width  = m_latestHeatmapW;
    height = m_latestHeatmapH;
    return true;
}


// ============================================================================
// Inference loop (runs on m_thread)
// ============================================================================

void TensorRTVisionSource::inferenceLoop()
{
    float* const hInputF      = static_cast<float*>(m_impl->hInput.ptr);
    float* const hHeatmapF    = static_cast<float*>(m_impl->hHeatmap.ptr);
    float* const hOffsetF     = static_cast<float*>(m_impl->hOffset.ptr);
    float* const hExistF      = static_cast<float*>(m_impl->hExist.ptr);
    float* const hPalmInputF  = static_cast<float*>(m_impl->hPalmInput.ptr);
    float* const hPalmScoresF = static_cast<float*>(m_impl->hPalmScores.ptr);

    while(m_running.load(std::memory_order_acquire))
    {
        if(m_resetRequested.exchange(false, std::memory_order_acq_rel))
        {
            m_candidates.clear();
            m_confirmedDarts.clear();
            m_mode.store(DetectionMode::Detecting, std::memory_order_relaxed);
            m_handStreak.store(0, std::memory_order_relaxed);
            m_clearStreak.store(0, std::memory_order_relaxed);
            for(uint32_t i = 0; i < EXPECTED_CAMERA_COUNT; i++) m_palmRecent[i] = false;
            m_boardClear.store(true, std::memory_order_release);
            {
                std::lock_guard<std::mutex> lock(m_eventMutex);
                std::queue<std::pair<float, float>> empty;
                std::swap(m_newDartEvents, empty);
            }
        }

        // Pull pre-warped frames from the camera capture threads (same as
        // the Hailo source — warp happens once per camera, then this loop
        // just copies bytes).
        uint32_t camCount = getCameraCount();
        uint32_t contributing = 0;
        for(uint32_t i = 0; i < N_CAMS; i++)
        {
            m_impl->warped[i] = cv::Mat();
            if(i >= camCount) continue;
            if(!getCameraWarpedFrame(i, m_impl->cameraFrames[i])) continue;

            const CameraFrame& cf = m_impl->cameraFrames[i];
            if(cf.pixels.empty()) continue;

            m_impl->warped[i] = cv::Mat(cf.height, cf.width, CV_8UC3,
                                        const_cast<uint8_t*>(cf.pixels.data()),
                                        cf.stride);
            contributing++;
        }

        if(contributing == 0)
        {
            // Cameras not ready yet — idle briefly.
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }

        // ----- Build the 10-channel NCHW float input tensor in pinned host memory -----
        // Channels 0..8: camera[c]'s RGB plane (/255 normalization).
        // Channel 9:    Gaussian mask over confirmed dart tips — held all-zero
        //               while in Removing so the AR model runs unconditioned
        //               and we can scan the heatmap for any leftover peak.
        //
        // Frames arrive in RGB order — camera_api.cpp:186 does BGR→RGB at
        // capture time before publishing to latestRaw / latestWarped. The
        // training script matches this (cv2.imread + cv2.cvtColor(BGR2RGB)),
        // so we pack channels in src order with no swap.
        for(uint32_t cam = 0; cam < N_CAMS; cam++)
        {
            const cv::Mat& w = m_impl->warped[cam];
            float* planeR = hInputF + (3u * cam + 0u) * PLANE_FLOATS;  // ONNX R
            float* planeG = hInputF + (3u * cam + 1u) * PLANE_FLOATS;  // ONNX G
            float* planeB = hInputF + (3u * cam + 2u) * PLANE_FLOATS;  // ONNX B

            if(w.empty())
            {
                std::memset(planeR, 0, PLANE_FLOATS * sizeof(float));
                std::memset(planeG, 0, PLANE_FLOATS * sizeof(float));
                std::memset(planeB, 0, PLANE_FLOATS * sizeof(float));
                continue;
            }

            constexpr float INV_255 = 1.0f / 255.0f;
            for(uint32_t r = 0; r < INPUT_H; r++)
            {
                const uint8_t* src = w.ptr(r);        // RGB, 3 bytes/pixel
                float* rowR = planeR + r * INPUT_W;
                float* rowG = planeG + r * INPUT_W;
                float* rowB = planeB + r * INPUT_W;
                for(uint32_t c = 0; c < INPUT_W; c++)
                {
                    rowR[c] = src[3 * c + 0] * INV_255;
                    rowG[c] = src[3 * c + 1] * INV_255;
                    rowB[c] = src[3 * c + 2] * INV_255;
                }
            }
        }

        // Conditioning mask (channel 9). Always start at zero. In Detecting,
        // rasterize a Gaussian for every confirmed dart so the AR model only
        // hunts for the next one. In Removing, leave the mask zero so the
        // model produces an unconditioned heatmap that we can scan for any
        // remaining dart.
        float* maskPlane = hInputF + 9u * PLANE_FLOATS;
        std::memset(maskPlane, 0, PLANE_FLOATS * sizeof(float));
        if(m_mode.load(std::memory_order_relaxed) == DetectionMode::Detecting)
        {
            for(const PolarDart& d : m_confirmedDarts)
            {
                addGaussianToPlane(maskPlane, d.templateX, d.templateY);
            }
        }

        // ----- Round-robin palm input pack -----
        // One camera per cycle. We grab the *raw* (unwarped) frame for that
        // camera — the perspective warp used for dart detection flattens
        // the board plane and shears anything in front of it, which makes
        // hands unrecognizable to a palm detector trained on natural photos.
        //
        // Preprocessing (verified against the actual ONNX graph + a Python
        // run of the model on a captured frame):
        //   - Layout: NHWC, i.e. interleaved RGB pixels. The BlazePalm
        //     ONNX input is [1, 192, 192, 3] — channels last, NOT the
        //     [1, C, H, W] convention the dart model uses. The graph's
        //     first op is a Transpose that converts NHWC→NCHW for the
        //     internal convs; if we feed planar CHW the transpose
        //     mangles our data into garbage and the network produces
        //     moderate-confidence noise everywhere.
        //   - Channel order: RGB. camera_api.cpp:186 already did BGR→RGB
        //     at capture time, so we pack bytes in src order with no swap.
        //   - Value range: [0, 1] for *full* BlazePalm. The lite model wanted
        //     [-1, 1] but the full export's training contract is different —
        //     feeding [-1, 1] produced saturated logits (sigmoid≈1.0) on
        //     every frame regardless of image content. Verified empirically
        //     by running the ONNX in onnxruntime with both ranges; only
        //     [0, 1] gives sane logits on real captures. So we apply
        //     p / 255.0 here.
        const uint32_t palmCam = m_palmFrameCounter++ % EXPECTED_CAMERA_COUNT;
        bool palmRanThisCycle = false;
        if(palmCam < camCount
        && getCameraFrame(palmCam, m_impl->palmRawFrame)
        && !m_impl->palmRawFrame.pixels.empty()
        && m_impl->palmRawFrame.width  > 0
        && m_impl->palmRawFrame.height > 0)
        {
            cv::Mat raw(m_impl->palmRawFrame.height,
                        m_impl->palmRawFrame.width,
                        CV_8UC3,
                        const_cast<uint8_t*>(m_impl->palmRawFrame.pixels.data()),
                        m_impl->palmRawFrame.stride);

            cv::resize(raw, m_impl->palmResized,
                       cv::Size(static_cast<int>(PALM_INPUT_W),
                                static_cast<int>(PALM_INPUT_H)),
                       0, 0, cv::INTER_LINEAR);

            // cv::Mat is already HWC-interleaved (one byte each: R, G, B,
            // R, G, B, ...). Destination tensor wants the same layout, so
            // it's a straight per-byte float convert with normalization.
            // Row-stride-safe in case OpenCV pads rows.
            constexpr float INV_255 = 1.0f / 255.0f;
            const uint32_t bytesPerRow = PALM_INPUT_W * 3u;
            for(uint32_t r = 0; r < PALM_INPUT_H; r++)
            {
                const uint8_t* src = m_impl->palmResized.ptr(r);  // RGB
                float*         dst = hPalmInputF + r * bytesPerRow;
                for(uint32_t b = 0; b < bytesPerRow; b++)
                {
                    dst[b] = src[b] * INV_255;
                }
            }
            palmRanThisCycle = true;
        }

        // ----- Inference -----
        // Issue both H2D copies first, then both enqueueV3s, then both D2H
        // copies. Because the two engines run on independent streams, the
        // GPU can overlap palm work with the much heavier U-Net. We don't
        // sync until both pipelines have submitted their D2H.
        if(cudaMemcpyAsync(m_impl->dInput.ptr, hInputF,
                           INPUT_FLOATS * sizeof(float),
                           cudaMemcpyHostToDevice, m_impl->stream) != cudaSuccess)
        {
            LOG_WARNING(VISION_LOG_ID, "cudaMemcpyAsync H2D failed");
            continue;
        }

        if(palmRanThisCycle)
        {
            if(cudaMemcpyAsync(m_impl->dPalmInput.ptr, hPalmInputF,
                               PALM_INPUT_FLOATS * sizeof(float),
                               cudaMemcpyHostToDevice, m_impl->palmStream) != cudaSuccess)
            {
                LOG_WARNING(VISION_LOG_ID, "cudaMemcpyAsync palm H2D failed");
                palmRanThisCycle = false;
            }
        }

        if(!m_impl->context->enqueueV3(m_impl->stream))
        {
            LOG_WARNING(VISION_LOG_ID, "enqueueV3 failed");
            continue;
        }

        if(palmRanThisCycle && !m_impl->palmContext->enqueueV3(m_impl->palmStream))
        {
            LOG_WARNING(VISION_LOG_ID, "enqueueV3 (palm) failed");
            palmRanThisCycle = false;
        }

        cudaMemcpyAsync(hHeatmapF, m_impl->dHeatmap.ptr,
                        HEATMAP_FLOATS * sizeof(float),
                        cudaMemcpyDeviceToHost, m_impl->stream);
        cudaMemcpyAsync(hOffsetF,  m_impl->dOffset.ptr,
                        OFFSET_FLOATS * sizeof(float),
                        cudaMemcpyDeviceToHost, m_impl->stream);
        cudaMemcpyAsync(hExistF,   m_impl->dExist.ptr,
                        sizeof(float),
                        cudaMemcpyDeviceToHost, m_impl->stream);

        if(palmRanThisCycle)
        {
            cudaMemcpyAsync(hPalmScoresF, m_impl->dPalmScores.ptr,
                            PALM_SCORES_FLOATS * sizeof(float),
                            cudaMemcpyDeviceToHost, m_impl->palmStream);
            // dPalmBoxes is bound but never copied back — we only need
            // presence, never location.
        }

        if(cudaStreamSynchronize(m_impl->stream) != cudaSuccess)
        {
            LOG_WARNING(VISION_LOG_ID, "cudaStreamSynchronize failed");
            continue;
        }

        // ----- Decode palm result and update per-camera memory -----
        bool palmDetectedThisCycle = false;
        if(palmRanThisCycle)
        {
            if(cudaStreamSynchronize(m_impl->palmStream) != cudaSuccess)
            {
                LOG_WARNING(VISION_LOG_ID, "cudaStreamSynchronize (palm) failed");
            }
            else
            {
                // Track the top-3 logits so we can tell saturation cases
                // apart in the diagnostic badge. A real hand fires a
                // cluster of high anchors; an isolated false positive
                // is one big logit with the rest near -∞.
                float t1 = -1e30f, t2 = -1e30f, t3 = -1e30f;
                for(uint32_t i = 0; i < PALM_SCORES_FLOATS; i++)
                {
                    const float v = hPalmScoresF[i];
                    if(v > t1)      { t3 = t2; t2 = t1; t1 = v; }
                    else if(v > t2) { t3 = t2; t2 = v; }
                    else if(v > t3) { t3 = v; }
                }
                m_palmTop1.store(t1, std::memory_order_relaxed);
                m_palmTop2.store(t2, std::memory_order_relaxed);
                m_palmTop3.store(t3, std::memory_order_relaxed);

                const float bestProb = 1.0f / (1.0f + std::exp(-t1));
                m_lastPalmScore.store(bestProb, std::memory_order_relaxed);
                palmDetectedThisCycle = (bestProb >= PALM_PRESENCE_THRESHOLD);
            }
        }

        // Update the per-camera last-result memory. A held dart can occlude
        // the hand from one or two cameras — keeping per-camera memory and
        // OR-ing the slots in handleInferenceOutputs makes the state
        // machine robust against that, which a "consecutive frames" rule
        // over the raw round-robin output would not be.
        m_palmRecent[palmCam] = palmDetectedThisCycle;
        bool handPresent = false;
        for(uint32_t i = 0; i < EXPECTED_CAMERA_COUNT; i++)
        {
            if(m_palmRecent[i]) { handPresent = true; break; }
        }
        m_lastHandPresent.store(handPresent, std::memory_order_relaxed);

        handleInferenceOutputs(handPresent);
    }
}


// ============================================================================
// Output decode + streak tracking
// ============================================================================

void TensorRTVisionSource::handleInferenceOutputs(bool handPresent)
{
    const float* heatmapLogits = static_cast<const float*>(m_impl->hHeatmap.ptr);
    const float* offset        = static_cast<const float*>(m_impl->hOffset.ptr);
    const float  existLogit    = *static_cast<const float*>(m_impl->hExist.ptr);

    // ----- Find argmax of heatmap logits (matches sigmoid argmax) -----
    size_t bestIdx = 0;
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
        std::lock_guard<std::mutex> lock(m_heatmapMutex);
        m_latestHeatmap  = std::move(sigmoided);
        m_latestHeatmapW = OUTPUT_W;
        m_latestHeatmapH = OUTPUT_H;
    }

    // ----- Gate on exist_logit and sigmoid(peak) -----
    const float bestProb = 1.0f / (1.0f + std::exp(-bestLogit));
    m_lastHeatmapPeak.store(bestProb, std::memory_order_relaxed);
    m_lastExistLogit.store(existLogit, std::memory_order_relaxed);
    const bool hasDetection = (existLogit >= EXIST_THRESHOLD)
                           && (bestProb   >= HEATMAP_THRESHOLD);

    // The board-clear check used by the Removing state needs only the
    // sigmoid threshold — exist_logit and the catch-ring radius gate are
    // skipped so a leftover dart anywhere on the heatmap (even just outside
    // the wire) blocks the cleared signal.
    const bool anyPeakAboveThreshold = (bestProb >= HEATMAP_THRESHOLD);
    m_lastPeakAboveThresh.store(anyPeakAboveThreshold, std::memory_order_relaxed);

    // ----- State machine: Detecting <-> Removing -----
    // m_boardClear is driven exclusively from here. While Removing it stays
    // false; when CLEAR_CONFIRM_FRAMES of clean cycles pass, we flip back
    // to Detecting and set it true.
    if(m_mode.load(std::memory_order_relaxed) == DetectionMode::Removing)
    {
        const bool clean = !handPresent && !anyPeakAboveThreshold;
        if(clean)
        {
            const int newClear = m_clearStreak.load(std::memory_order_relaxed) + 1;
            m_clearStreak.store(newClear, std::memory_order_relaxed);
            if(newClear >= CLEAR_CONFIRM_FRAMES)
            {
                m_mode.store(DetectionMode::Detecting, std::memory_order_relaxed);
                m_handStreak.store(0, std::memory_order_relaxed);
                m_clearStreak.store(0, std::memory_order_relaxed);
                m_boardClear.store(true, std::memory_order_release);
            }
        }
        else
        {
            m_clearStreak.store(0, std::memory_order_relaxed);
        }
        // No candidate tracking, no dart emission while Removing.
        return;
    }

    // ----- Detecting: check for hand entry first ------------------------
    if(handPresent)
    {
        const int newHand = m_handStreak.load(std::memory_order_relaxed) + 1;
        m_handStreak.store(newHand, std::memory_order_relaxed);
        if(newHand >= HAND_ENTER_FRAMES)
        {
            // Enter Removing: drop all dart state so the AR model runs
            // unconditioned next cycle and we can scan the heatmap for any
            // remaining dart. m_boardClear stays false until the clear
            // streak completes.
            m_mode.store(DetectionMode::Removing, std::memory_order_relaxed);
            m_candidates.clear();
            m_confirmedDarts.clear();
            m_clearStreak.store(0, std::memory_order_relaxed);
            m_boardClear.store(false, std::memory_order_release);
            return;
        }
    }
    else
    {
        m_handStreak.store(0, std::memory_order_relaxed);
    }

    // ----- Decode sub-pixel peak -----
    const uint32_t r = static_cast<uint32_t>(bestIdx / OUTPUT_W);
    const uint32_t c = static_cast<uint32_t>(bestIdx % OUTPUT_W);
    const size_t   dxIdx = 0u * (HEATMAP_FLOATS) + bestIdx;   // offset plane 0 = dx
    const size_t   dyIdx = 1u * (HEATMAP_FLOATS) + bestIdx;   // offset plane 1 = dy
    const float    peakR = static_cast<float>(r) + 0.5f + offset[dyIdx];
    const float    peakC = static_cast<float>(c) + 0.5f + offset[dxIdx];

    // ----- Convert heatmap-cell coords → polar (angle, normRadius) -----
    PolarDart detected{};
    bool      produced = false;
    if(hasDetection)
    {
        const float tx = peakC * HEATMAP_TO_TEMPLATE;
        const float ty = peakR * HEATMAP_TO_TEMPLATE;
        const float dx = tx - TEMPLATE_CENTER;
        const float dy = ty - TEMPLATE_CENTER;
        const float dist = std::sqrt(dx * dx + dy * dy);
        const float normR = dist / BOARD_RADIUS_PX;

        if(normR <= MAX_DETECT_RADIUS)
        {
            detected.angle             = static_cast<float>(std::atan2(dy, dx) * 180.0 / M_PI);
            detected.normalizedRadius  = normR;
            detected.templateX         = tx;
            detected.templateY         = ty;
            produced = true;
        }
    }

    // Board is clear iff no unmasked dart was detected AND no darts are
    // already confirmed. (The model masks confirmed darts, so it can't
    // "see" them — board-clear derives from what we know, not what it
    // just predicted.)
    m_boardClear.store(!produced && m_confirmedDarts.empty(),
                       std::memory_order_release);

    // ----- Streak tracker — one detection per frame simplifies matching -----
    const float matchR2 = DART_MATCH_RADIUS_PX * DART_MATCH_RADIUS_PX;
    auto distSqTemplate = [](const PolarDart& a, const PolarDart& b) -> float
    {
        const float ddx = a.templateX - b.templateX;
        const float ddy = a.templateY - b.templateY;
        return ddx * ddx + ddy * ddy;
    };

    bool emittedThisFrame = false;
    PolarDart emitted{};

    if(produced)
    {
        int   bestCand = -1;
        float bestD2   = matchR2;
        for(size_t i = 0; i < m_candidates.size(); i++)
        {
            const float d2 = distSqTemplate(detected, m_candidates[i].polar);
            if(d2 < bestD2)
            {
                bestD2   = d2;
                bestCand = static_cast<int>(i);
            }
        }

        if(bestCand >= 0)
        {
            m_candidates[bestCand].polar = detected;
            m_candidates[bestCand].streak++;
            if(m_candidates[bestCand].streak >= CONFIRM_FRAMES
            && !m_candidates[bestCand].emitted)
            {
                m_candidates[bestCand].emitted = true;
                emitted = detected;
                emittedThisFrame = true;
            }
        }
        else
        {
            CandidateDart cd;
            cd.polar   = detected;
            cd.streak  = 1;
            cd.emitted = false;
            m_candidates.push_back(cd);
        }
    }

    // Drop candidates that didn't get a hit this frame (either because the
    // detection was elsewhere, or because there was no detection at all).
    // With one detection per frame, any candidate not matched above is gone.
    for(int i = static_cast<int>(m_candidates.size()) - 1; i >= 0; i--)
    {
        const bool matched = produced
                          && distSqTemplate(m_candidates[i].polar, detected) < matchR2;
        if(!matched) m_candidates.erase(m_candidates.begin() + i);
    }

    if(emittedThisFrame)
    {
        // A dart was confirmed — add to the conditioning set so the next
        // inference sees it in the mask and hunts for the next dart.
        m_confirmedDarts.push_back(emitted);
        m_boardClear.store(false, std::memory_order_release);

        std::lock_guard<std::mutex> lock(m_eventMutex);
        m_newDartEvents.emplace(emitted.angle, emitted.normalizedRadius);
    }

    // Surface the largest candidate streak so the debug badge can show
    // whether we're accumulating frames toward a confirmation. If the
    // heatmap is hot but this stays stuck below CONFIRM_FRAMES, we know
    // the streak isn't completing (e.g. position wobble or transient gate
    // failures) rather than the gates being unreachable.
    int maxStreak = 0;
    for(const auto& c : m_candidates)
    {
        if(c.streak > maxStreak) maxStreak = c.streak;
    }
    m_lastCandidateStreak.store(maxStreak, std::memory_order_relaxed);
}
