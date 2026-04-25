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


// ============================================================================
// Constants
// ============================================================================

namespace
{
    // Paths relative to the executable — the ONNX ships alongside the cached
    // .trt engine. Engine is rebuilt automatically when the ONNX is newer.
    constexpr const char* ONNX_PATH   = "./vision/multicam_unet_ar/multicam_unet_ar.onnx";
    constexpr const char* ENGINE_PATH = "./vision/multicam_unet_ar/multicam_unet_ar.trt";

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
    constexpr const char* PALM_ENGINE_PATH = "./vision/palm_detection/blazepalm.trt";

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

    // Sigmoid threshold on the max anchor score. 0.5 is the BlazePalm
    // default — palm detector is well-calibrated, no need to tune.
    constexpr float PALM_PRESENCE_THRESHOLD = 0.5f;
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
    // TRT progress monitor — called by the builder during engine build
    // so the UI can show something better than a black screen.
    //
    // TRT phases are hierarchical (parent phase -> child phases). The
    // top-level phase's step count is the closest thing to "overall
    // progress"; we track it and report child phase names as status
    // text for operator context.
    // ----------------------------------------------------------------
    class TrtProgressMonitor : public nvinfer1::IProgressMonitor
    {
        public:
            TrtProgressMonitor(std::atomic<float>& progressOut,
                               std::mutex& statusMutex,
                               std::string& statusOut,
                               std::atomic<bool>& abortFlag)
                : m_progress(progressOut)
                , m_statusMutex(statusMutex)
                , m_status(statusOut)
                , m_abort(abortFlag)
            {}

            void phaseStart(char const* phaseName,
                            char const* parentPhase,
                            int32_t nbSteps) noexcept override
            {
                // Top-level phase = no parent. Use its step count as the
                // denominator for "overall" progress.
                const bool topLevel = (parentPhase == nullptr) || (parentPhase[0] == '\0');
                if(topLevel)
                {
                    m_topSteps.store(nbSteps, std::memory_order_release);
                    m_topStep.store(0, std::memory_order_release);
                    m_progress.store(0.0f, std::memory_order_release);
                }

                if(phaseName)
                {
                    std::lock_guard<std::mutex> lock(m_statusMutex);
                    m_status = phaseName;
                }
            }

            bool stepComplete(char const* phaseName, int32_t step) noexcept override
            {
                (void)phaseName;
                // We only track top-level progress; sub-phase steps are
                // noise for the progress bar. But sub-phase NAMES are
                // still useful status text (set in phaseStart).
                const int32_t totalSteps = m_topSteps.load(std::memory_order_acquire);
                if(totalSteps > 0)
                {
                    // step indices in TRT are zero-based; nbSteps is total
                    // count, so fraction is (step+1)/nbSteps when called
                    // for a top-level step. Phases can nest, so we cap at 1.
                    const int32_t cur = std::min(step + 1, totalSteps);
                    m_topStep.store(cur, std::memory_order_release);
                    m_progress.store(static_cast<float>(cur) / static_cast<float>(totalSteps),
                                     std::memory_order_release);
                }
                return !m_abort.load(std::memory_order_acquire);
            }

            void phaseFinish(char const* phaseName) noexcept override
            {
                (void)phaseName;
                // Top-level finish → clamp progress to 1.0 so the bar
                // visibly completes even if nbSteps didn't land exactly.
                const int32_t totalSteps = m_topSteps.load(std::memory_order_acquire);
                if(totalSteps > 0 && m_topStep.load() >= totalSteps)
                {
                    m_progress.store(1.0f, std::memory_order_release);
                }
            }

        private:
            std::atomic<float>&   m_progress;
            std::mutex&           m_statusMutex;
            std::string&          m_status;
            std::atomic<bool>&    m_abort;
            std::atomic<int32_t>  m_topSteps{0};
            std::atomic<int32_t>  m_topStep{0};
    };


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


    // Build an FP16 engine from the ONNX and serialize it to disk. Logs the
    // (multi-second) build cost so the first-run delay doesn't look like a
    // hang. Optionally accepts a progress monitor so the UI can draw a
    // progress bar during the build.
    std::vector<uint8_t> buildEngineFromOnnx(const std::string& onnxPath,
                                             nvinfer1::IProgressMonitor* monitor)
    {
        LOG_INFO(VISION_LOG_ID,
                 "TensorRTVisionSource: building FP16 engine from {} "
                 "(first run only, typically 2-5 minutes on Orin Nano Super)",
                 onnxPath);

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

        if(builder->platformHasFastFp16())
        {
            config->setFlag(nvinfer1::BuilderFlag::kFP16);
        }
        else
        {
            LOG_WARNING(VISION_LOG_ID,
                        "TRT: platform reports no fast FP16 — falling back to FP32");
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
// Publishes progress + status through the atomics for the UI.
void TensorRTVisionSource::buildThreadMain()
{
    auto setStatus = [&](const std::string& s)
    {
        std::lock_guard<std::mutex> lock(m_buildStatusMutex);
        m_buildStatus = s;
    };
    auto fail = [&](const char* why)
    {
        LOG_ERROR(VISION_LOG_ID, "TensorRTVisionSource build failed: {}", why);
        setStatus(std::string("Failed: ") + why);
        m_buildState.store(BuildState::Failed, std::memory_order_release);
    };

    try
    {
        // ---- Load cached engine if present and fresh ------------------
        setStatus("Checking dart-detector cache");
        std::vector<uint8_t> planBytes;
        bool loadedFromCache = false;
        if(cachedEngineIsFresh(ENGINE_PATH, ONNX_PATH))
        {
            if(readFileBytes(ENGINE_PATH, planBytes))
            {
                LOG_INFO(VISION_LOG_ID,
                         "TensorRTVisionSource: loaded cached engine from {} ({} bytes)",
                         ENGINE_PATH, planBytes.size());
                loadedFromCache = true;
                // Cached load is fast — show a near-full bar so it feels
                // snappy instead of jumping from 0% to 100% instantly.
                m_buildProgress.store(0.8f, std::memory_order_release);
                setStatus("Loading cached dart detector");
            }
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
                            "Cached engine failed to deserialize "
                            "(likely GPU/TRT version mismatch) — rebuilding");
                planBytes.clear();
                loadedFromCache = false;
            }
        }

        // ---- Build from ONNX if there was no cached plan --------------
        if(!loadedFromCache)
        {
            setStatus("Building dart detector");
            m_buildProgress.store(0.0f, std::memory_order_release);

            TrtProgressMonitor monitor(m_buildProgress,
                                       m_buildStatusMutex,
                                       m_buildStatus,
                                       m_buildAbort);
            planBytes = buildEngineFromOnnx(ONNX_PATH, &monitor);
            if(planBytes.empty() || m_buildAbort.load(std::memory_order_acquire))
            {
                return fail("engine build");
            }

            // Cache the freshly built engine for next launch. Non-fatal
            // if the write fails (e.g. read-only fs) — just means we
            // rebuild next time.
            setStatus("Caching engine to disk");
            std::ofstream f(ENGINE_PATH, std::ios::binary);
            if(f.write(reinterpret_cast<const char*>(planBytes.data()),
                       static_cast<std::streamsize>(planBytes.size())))
            {
                LOG_INFO(VISION_LOG_ID,
                         "TensorRTVisionSource: wrote engine cache to {}", ENGINE_PATH);
            }
            else
            {
                LOG_WARNING(VISION_LOG_ID,
                            "TensorRTVisionSource: failed to write engine cache to {} "
                            "(errno={}); will rebuild next run",
                            ENGINE_PATH, errno);
            }

            m_impl->engine.reset(m_impl->runtime->deserializeCudaEngine(
                planBytes.data(), planBytes.size()));
            if(!m_impl->engine) return fail("deserializeCudaEngine on fresh plan");
        }

        // Bail out early if shutdown was requested mid-build.
        if(m_buildAbort.load(std::memory_order_acquire))
        {
            setStatus("Aborted");
            m_buildState.store(BuildState::Failed, std::memory_order_release);
            return;
        }

        // ---- Context + buffers + bindings ----------------------------
        setStatus("Allocating GPU buffers");
        m_buildProgress.store(0.95f, std::memory_order_release);

        m_impl->context.reset(m_impl->engine->createExecutionContext());
        if(!m_impl->context) return fail("createExecutionContext");

        if(!m_impl->dInput.allocate(INPUT_FLOATS * sizeof(float))
        || !m_impl->dHeatmap.allocate(HEATMAP_FLOATS * sizeof(float))
        || !m_impl->dOffset.allocate(OFFSET_FLOATS * sizeof(float))
        || !m_impl->dExist.allocate(sizeof(float))
        || !m_impl->hInput.allocate(INPUT_FLOATS * sizeof(float))
        || !m_impl->hHeatmap.allocate(HEATMAP_FLOATS * sizeof(float))
        || !m_impl->hOffset.allocate(OFFSET_FLOATS * sizeof(float))
        || !m_impl->hExist.allocate(sizeof(float)))
        {
            return fail("CUDA buffer allocation");
        }

        m_impl->context->setTensorAddress(INPUT_NAME,  m_impl->dInput.ptr);
        m_impl->context->setTensorAddress(OUT_HEATMAP, m_impl->dHeatmap.ptr);
        m_impl->context->setTensorAddress(OUT_OFFSET,  m_impl->dOffset.ptr);
        m_impl->context->setTensorAddress(OUT_EXIST,   m_impl->dExist.ptr);

        if(m_buildAbort.load(std::memory_order_acquire))
        {
            setStatus("Aborted");
            m_buildState.store(BuildState::Failed, std::memory_order_release);
            return;
        }

        // ---- BlazePalm: same load-or-build flow on a second engine ----
        // Re-uses the runtime, builder logger, and progress monitor. The
        // palm engine is small (~5 MB) so the build phase is fast.
        setStatus("Checking palm-detector cache");
        std::vector<uint8_t> palmPlanBytes;
        bool palmFromCache = false;
        if(cachedEngineIsFresh(PALM_ENGINE_PATH, PALM_ONNX_PATH))
        {
            if(readFileBytes(PALM_ENGINE_PATH, palmPlanBytes))
            {
                LOG_INFO(VISION_LOG_ID,
                         "TensorRTVisionSource: loaded cached palm engine from {} "
                         "({} bytes)",
                         PALM_ENGINE_PATH, palmPlanBytes.size());
                palmFromCache = true;
            }
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
            setStatus("Building palm detector");
            TrtProgressMonitor palmMonitor(m_buildProgress,
                                           m_buildStatusMutex,
                                           m_buildStatus,
                                           m_buildAbort);
            palmPlanBytes = buildEngineFromOnnx(PALM_ONNX_PATH, &palmMonitor);
            if(palmPlanBytes.empty() || m_buildAbort.load(std::memory_order_acquire))
            {
                return fail("palm engine build");
            }

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

        setStatus("Allocating palm-detector buffers");
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

        m_impl->palmContext->setTensorAddress(PALM_INPUT_NAME, m_impl->dPalmInput.ptr);
        m_impl->palmContext->setTensorAddress(PALM_OUT_SCORES, m_impl->dPalmScores.ptr);
        m_impl->palmContext->setTensorAddress(PALM_OUT_BOXES,  m_impl->dPalmBoxes.ptr);

        if(cudaStreamCreate(&m_impl->palmStream) != cudaSuccess)
        {
            return fail("cudaStreamCreate (palm)");
        }

        // ---- Launch the inference thread -----------------------------
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
        fail("exception");
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


float TensorRTVisionSource::getInitProgress() const
{
    return m_buildProgress.load(std::memory_order_acquire);
}


std::string TensorRTVisionSource::getInitStatus() const
{
    std::lock_guard<std::mutex> lock(m_buildStatusMutex);
    return m_buildStatus;
}


std::string TensorRTVisionSource::getDetectionStatus() const
{
    const DetectionMode mode  = m_mode.load(std::memory_order_relaxed);
    const int handStreak      = m_handStreak.load(std::memory_order_relaxed);
    const int clearStreak     = m_clearStreak.load(std::memory_order_relaxed);

    if(mode == DetectionMode::Removing)
    {
        return "Removing (clear " + std::to_string(clearStreak)
             + "/" + std::to_string(CLEAR_CONFIRM_FRAMES) + ")";
    }

    if(handStreak > 0)
    {
        return "Detecting (entering " + std::to_string(handStreak)
             + "/" + std::to_string(HAND_ENTER_FRAMES) + ")";
    }
    return "Detecting";
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
        // BlazePalm's preprocessing contract (per Google's model card):
        //   - Channel order: RGB. camera_api.cpp:186 already did BGR→RGB at
        //     capture time, so we pack channels in src order with no swap.
        //   - Value range: [-1, 1]. The formula is (pixel / 127.5) - 1.0.
        //     NOT [0, 1] — that's the dart model's convention, different
        //     model, different contract.
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

            float* palmR = hPalmInputF + 0u * PALM_INPUT_H * PALM_INPUT_W;
            float* palmG = hPalmInputF + 1u * PALM_INPUT_H * PALM_INPUT_W;
            float* palmB = hPalmInputF + 2u * PALM_INPUT_H * PALM_INPUT_W;
            constexpr float INV_127_5 = 1.0f / 127.5f;
            for(uint32_t r = 0; r < PALM_INPUT_H; r++)
            {
                const uint8_t* src = m_impl->palmResized.ptr(r);  // RGB
                float* rowR = palmR + r * PALM_INPUT_W;
                float* rowG = palmG + r * PALM_INPUT_W;
                float* rowB = palmB + r * PALM_INPUT_W;
                for(uint32_t c = 0; c < PALM_INPUT_W; c++)
                {
                    rowR[c] = src[3 * c + 0] * INV_127_5 - 1.0f;
                    rowG[c] = src[3 * c + 1] * INV_127_5 - 1.0f;
                    rowB[c] = src[3 * c + 2] * INV_127_5 - 1.0f;
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
                float bestLogit = hPalmScoresF[0];
                for(uint32_t i = 1; i < PALM_SCORES_FLOATS; i++)
                {
                    if(hPalmScoresF[i] > bestLogit) bestLogit = hPalmScoresF[i];
                }
                const float bestProb = 1.0f / (1.0f + std::exp(-bestLogit));
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
    const bool hasDetection = (existLogit >= EXIST_THRESHOLD)
                           && (bestProb   >= HEATMAP_THRESHOLD);

    // The board-clear check used by the Removing state needs only the
    // sigmoid threshold — exist_logit and the catch-ring radius gate are
    // skipped so a leftover dart anywhere on the heatmap (even just outside
    // the wire) blocks the cleared signal.
    const bool anyPeakAboveThreshold = (bestProb >= HEATMAP_THRESHOLD);

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
}
