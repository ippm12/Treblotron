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
    // hang.
    std::vector<uint8_t> buildEngineFromOnnx(const std::string& onnxPath)
    {
        LOG_INFO(VISION_LOG_ID,
                 "TensorRTVisionSource: building FP16 engine from {} "
                 "(this takes 30-90 seconds on first run)",
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
    initializeCameraSystem();

    LOG_INFO(VISION_LOG_ID, "TensorRTVisionSource: creating CUDA stream");
    if(cudaStreamCreate(&m_impl->stream) != cudaSuccess)
    {
        LOG_ERROR(VISION_LOG_ID, "cudaStreamCreate failed");
        return STATUS_ERROR_GENERIC;
    }

    // Load or build the engine.
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
        }
    }

    m_impl->runtime.reset(nvinfer1::createInferRuntime(g_trtLogger));
    if(!m_impl->runtime)
    {
        LOG_ERROR(VISION_LOG_ID, "createInferRuntime failed");
        return STATUS_ERROR_GENERIC;
    }

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

    if(!loadedFromCache)
    {
        planBytes = buildEngineFromOnnx(ONNX_PATH);
        if(planBytes.empty()) return STATUS_ERROR_GENERIC;

        // Cache the freshly built engine for next launch. Non-fatal if the
        // write fails (e.g. read-only filesystem) — just means we rebuild
        // next time.
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
        if(!m_impl->engine)
        {
            LOG_ERROR(VISION_LOG_ID, "deserializeCudaEngine failed on fresh plan");
            return STATUS_ERROR_GENERIC;
        }
    }

    m_impl->context.reset(m_impl->engine->createExecutionContext());
    if(!m_impl->context)
    {
        LOG_ERROR(VISION_LOG_ID, "createExecutionContext failed");
        return STATUS_ERROR_GENERIC;
    }

    // Allocate device + pinned buffers for the four tensors.
    if(!m_impl->dInput.allocate(INPUT_FLOATS * sizeof(float))
    || !m_impl->dHeatmap.allocate(HEATMAP_FLOATS * sizeof(float))
    || !m_impl->dOffset.allocate(OFFSET_FLOATS * sizeof(float))
    || !m_impl->dExist.allocate(sizeof(float))
    || !m_impl->hInput.allocate(INPUT_FLOATS * sizeof(float))
    || !m_impl->hHeatmap.allocate(HEATMAP_FLOATS * sizeof(float))
    || !m_impl->hOffset.allocate(OFFSET_FLOATS * sizeof(float))
    || !m_impl->hExist.allocate(sizeof(float)))
    {
        LOG_ERROR(VISION_LOG_ID, "CUDA buffer allocation failed");
        return STATUS_ERROR_GENERIC;
    }

    // Bind tensor addresses — these persist across enqueueV3 calls.
    m_impl->context->setTensorAddress(INPUT_NAME,  m_impl->dInput.ptr);
    m_impl->context->setTensorAddress(OUT_HEATMAP, m_impl->dHeatmap.ptr);
    m_impl->context->setTensorAddress(OUT_OFFSET,  m_impl->dOffset.ptr);
    m_impl->context->setTensorAddress(OUT_EXIST,   m_impl->dExist.ptr);

    m_running.store(true, std::memory_order_release);
    m_thread = std::thread(&TensorRTVisionSource::inferenceLoop, this);

    LOG_INFO(VISION_LOG_ID, "TensorRTVisionSource initialized");
    return STATUS_OK;
    }
    catch(const std::exception& e)
    {
        LOG_ERROR(VISION_LOG_ID, "TensorRTVisionSource::init threw: {}", e.what());
        return STATUS_ERROR_GENERIC;
    }
    catch(...)
    {
        LOG_ERROR(VISION_LOG_ID, "TensorRTVisionSource::init threw unknown exception");
        return STATUS_ERROR_GENERIC;
    }
}


void TensorRTVisionSource::shutdown()
{
    m_running.store(false, std::memory_order_release);
    if(m_thread.joinable()) m_thread.join();

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

    if(m_impl->stream)
    {
        cudaStreamDestroy(m_impl->stream);
        m_impl->stream = nullptr;
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
    float* const hInputF   = static_cast<float*>(m_impl->hInput.ptr);
    float* const hHeatmapF = static_cast<float*>(m_impl->hHeatmap.ptr);
    float* const hOffsetF  = static_cast<float*>(m_impl->hOffset.ptr);
    float* const hExistF   = static_cast<float*>(m_impl->hExist.ptr);

    while(m_running.load(std::memory_order_acquire))
    {
        if(m_resetRequested.exchange(false, std::memory_order_acq_rel))
        {
            m_candidates.clear();
            m_confirmedDarts.clear();
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
        // Channels 0..8: camera[c]'s RGB plane (BGR→RGB swap, /255 normalization).
        // Channel 9:    Gaussian mask over confirmed dart tips.
        //
        // OpenCV camera frames are BGR (the training script does cv2.imread
        // → cv2.cvtColor(BGR2RGB)), so we swap bytes here to match.
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
                const uint8_t* src = w.ptr(r);        // BGR, 3 bytes/pixel
                float* rowR = planeR + r * INPUT_W;
                float* rowG = planeG + r * INPUT_W;
                float* rowB = planeB + r * INPUT_W;
                for(uint32_t c = 0; c < INPUT_W; c++)
                {
                    rowB[c] = src[3 * c + 0] * INV_255;
                    rowG[c] = src[3 * c + 1] * INV_255;
                    rowR[c] = src[3 * c + 2] * INV_255;
                }
            }
        }

        // Conditioning mask (channel 9).
        float* maskPlane = hInputF + 9u * PLANE_FLOATS;
        std::memset(maskPlane, 0, PLANE_FLOATS * sizeof(float));
        for(const PolarDart& d : m_confirmedDarts)
        {
            addGaussianToPlane(maskPlane, d.templateX, d.templateY);
        }

        // ----- Inference -----
        if(cudaMemcpyAsync(m_impl->dInput.ptr, hInputF,
                           INPUT_FLOATS * sizeof(float),
                           cudaMemcpyHostToDevice, m_impl->stream) != cudaSuccess)
        {
            LOG_WARNING(VISION_LOG_ID, "cudaMemcpyAsync H2D failed");
            continue;
        }

        if(!m_impl->context->enqueueV3(m_impl->stream))
        {
            LOG_WARNING(VISION_LOG_ID, "enqueueV3 failed");
            continue;
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

        if(cudaStreamSynchronize(m_impl->stream) != cudaSuccess)
        {
            LOG_WARNING(VISION_LOG_ID, "cudaStreamSynchronize failed");
            continue;
        }

        handleInferenceOutputs();
    }
}


// ============================================================================
// Output decode + streak tracking
// ============================================================================

void TensorRTVisionSource::handleInferenceOutputs()
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
