/**
 * trt_backend.cpp
 *
 * TensorRT inference backend for Jetson (Orin Nano Super). Carries over the
 * engine build/cache/bind logic that used to live inline in
 * tensorrt_vision_source.cpp, now behind the InferenceBackend interface so
 * DartDetector doesn't have to know about it.
 *
 * Each model gets its own CUDA stream, so submit()/wait() on two models
 * genuinely overlap on the GPU — that's how the palm detector hides behind the
 * much heavier U-Net.
 *
 * This file is only compiled when TREBLOTRON_INFER_BACKEND=tensorrt; the Windows
 * and Raspberry Pi builds skip it entirely so the TensorRT / CUDA headers only
 * need to be available on the Jetson.
 */

#include "detect/inference_backend.hpp"

#include <NvInfer.h>
#include <NvOnnxParser.h>
#include <cuda_runtime.h>

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>


namespace
{
    // Single source of truth for the workspace cap. 2 GiB is comfortably above
    // what these networks need and well under the Orin Nano Super's 8 GB
    // unified memory budget.
    constexpr size_t TRT_WORKSPACE_BYTES = 1ULL << 31;


    // ------------------------------------------------------------------
    // TRT logger — forwards everything above INFO through the project logger.
    // ------------------------------------------------------------------
    class TrtLogger : public nvinfer1::ILogger
    {
        public:
            void log(Severity sev, const char* msg) noexcept override
            {
                switch(sev)
                {
                    case Severity::kINTERNAL_ERROR:
                    case Severity::kERROR:
                        LOG_ERROR(DETECT_LOG_ID, "TRT: {}", msg);
                        break;
                    case Severity::kWARNING:
                        LOG_WARNING(DETECT_LOG_ID, "TRT: {}", msg);
                        break;
                    case Severity::kINFO:
                        LOG_INFO(DETECT_LOG_ID, "TRT: {}", msg);
                        break;
                    case Severity::kVERBOSE:
                        // Intentionally dropped — TRT is very chatty at verbose.
                        break;
                }
            }
    };

    TrtLogger g_trtLogger;


    /**
     * Bumps a step counter on every TRT build step so the loading screen can
     * show "still doing something" feedback during long-running phases like
     * timing tactics, where the fractional progress doesn't visibly move.
     * Also carries the abort flag — returning false from stepComplete is how
     * TRT is asked to unwind a build early.
     */
    class TrtProgressMonitor : public nvinfer1::IProgressMonitor
    {
        public:
            TrtProgressMonitor(const InferenceBackend::ProgressFn& fn,
                               const std::atomic<bool>& abortFlag)
                : m_fn(fn), m_abort(abortFlag) {}

            void phaseStart(char const* phaseName, char const* /*parentPhase*/,
                            int32_t /*nbSteps*/) noexcept override
            {
                if(phaseName) m_phase = phaseName;
            }

            bool stepComplete(char const* /*phaseName*/, int32_t /*step*/) noexcept override
            {
                m_iteration++;
                if(m_fn) m_fn(0.5f, m_iteration, m_phase);
                return !m_abort.load(std::memory_order_acquire);
            }

            void phaseFinish(char const* /*phaseName*/) noexcept override {}

        private:
            const InferenceBackend::ProgressFn& m_fn;
            const std::atomic<bool>&            m_abort;
            uint64_t                            m_iteration = 0;
            std::string                         m_phase;
    };


    /**
     * Returns true iff the engine exposes an I/O tensor with the given name.
     * setTensorAddress silently no-ops on unknown names, so we have to check
     * explicitly — otherwise a renamed export fails silently at runtime.
     */
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


    /** RAII for CUDA device allocations. Only malloc + free are needed here. */
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
            if(cudaMalloc(&ptr, n) != cudaSuccess) { ptr = nullptr; return false; }
            bytes = n;
            return true;
        }

        void free()
        {
            if(ptr) { cudaFree(ptr); ptr = nullptr; }
            bytes = 0;
        }
    };


    /** Pinned-host staging buffer — enables true async H2D/D2H copies. */
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


    /**
     * Cache key: rebuild whenever the ONNX is newer than the cached engine.
     * Deserialization failures (GPU/TRT version mismatch) also trigger a
     * rebuild further down.
     */
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


    /**
     * Build an engine from the ONNX and serialize it. `preferFp16` controls
     * precision — networks with deep activation chains (e.g. the full
     * BlazePalm) overflow FP16's +-65504 range and produce NaN, so they need
     * FP32. `fp32LayerNames` selectively pins individual layers to FP32 while
     * the rest stays at the requested precision.
     */
    std::vector<uint8_t> buildEngineFromOnnx(const std::string& onnxPath,
                                             nvinfer1::IProgressMonitor* monitor,
                                             bool preferFp16,
                                             const std::vector<std::string>& fp32LayerNames)
    {
        const char* precisionLabel = preferFp16 ? "FP16" : "FP32";
        LOG_INFO(DETECT_LOG_ID,
                 "trt backend: building {} engine from {} "
                 "(first run only, typically 2-5 minutes on Orin Nano Super)",
                 precisionLabel, onnxPath);

        std::unique_ptr<nvinfer1::IBuilder> builder(nvinfer1::createInferBuilder(g_trtLogger));
        if(!builder) { LOG_ERROR(DETECT_LOG_ID, "createInferBuilder failed"); return {}; }

        // TRT 10 uses strongly-typed explicit-batch networks by default —
        // passing 0 flags is correct.
        std::unique_ptr<nvinfer1::INetworkDefinition> network(builder->createNetworkV2(0));
        if(!network) { LOG_ERROR(DETECT_LOG_ID, "createNetworkV2 failed"); return {}; }

        std::unique_ptr<nvonnxparser::IParser> parser(
            nvonnxparser::createParser(*network, g_trtLogger));
        if(!parser) { LOG_ERROR(DETECT_LOG_ID, "nvonnxparser::createParser failed"); return {}; }

        if(!parser->parseFromFile(onnxPath.c_str(),
                                  static_cast<int>(nvinfer1::ILogger::Severity::kWARNING)))
        {
            for(int i = 0; i < parser->getNbErrors(); i++)
            {
                LOG_ERROR(DETECT_LOG_ID, "ONNX parse error: {}", parser->getError(i)->desc());
            }
            return {};
        }

        std::unique_ptr<nvinfer1::IBuilderConfig> config(builder->createBuilderConfig());
        if(!config) { LOG_ERROR(DETECT_LOG_ID, "createBuilderConfig failed"); return {}; }

        if(preferFp16)
        {
            if(builder->platformHasFastFp16())
            {
                config->setFlag(nvinfer1::BuilderFlag::kFP16);
            }
            else
            {
                LOG_WARNING(DETECT_LOG_ID,
                            "TRT: platform reports no fast FP16 — falling back to FP32");
            }
        }

        // Selectively pin per-layer precision. With the OBEY flag set TRT is
        // required to honor the constraint or fail the build — the alternative
        // (PREFER) silently falls back to FP16, which would re-introduce the
        // overflow we're trying to escape. Names match the exact ONNX node
        // name (e.g. "/m/exist_head/ReduceMax") so we don't accidentally catch
        // something like "/m/Squeeze_1" — that bit us once already.
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
                LOG_INFO(DETECT_LOG_ID, "TRT: pinned layer {} to FP32", lname);
                matched[matchIdx] = true;
            }

            for(size_t k = 0; k < fp32LayerNames.size(); k++)
            {
                if(!matched[k])
                {
                    LOG_WARNING(DETECT_LOG_ID,
                                "TRT: requested FP32 pin for layer {} not found in network "
                                "— check the ONNX node names",
                                fp32LayerNames[k]);
                }
            }
        }

        config->setMemoryPoolLimit(nvinfer1::MemoryPoolType::kWORKSPACE, TRT_WORKSPACE_BYTES);
        if(monitor) config->setProgressMonitor(monitor);

        std::unique_ptr<nvinfer1::IHostMemory> plan(
            builder->buildSerializedNetwork(*network, *config));
        if(!plan)
        {
            LOG_ERROR(DETECT_LOG_ID, "buildSerializedNetwork failed");
            return {};
        }

        std::vector<uint8_t> out(plan->size());
        std::memcpy(out.data(), plan->data(), plan->size());
        LOG_INFO(DETECT_LOG_ID, "trt backend: engine built ({} bytes)", out.size());
        return out;
    }


    // ------------------------------------------------------------------
    // Model
    // ------------------------------------------------------------------
    class TrtModel : public InferenceBackend::Model
    {
        public:
            ~TrtModel() override
            {
                m_context.reset();
                m_engine.reset();
                if(m_stream) cudaStreamDestroy(m_stream);
            }

            bool create(nvinfer1::IRuntime& runtime,
                        const std::vector<uint8_t>& plan,
                        const InferenceBackend::ModelSpec& spec)
            {
                m_engine.reset(runtime.deserializeCudaEngine(plan.data(), plan.size()));
                if(!m_engine) return false;

                for(const TensorSpec& t : spec.inputs)
                {
                    if(!engineHasIOTensor(m_engine.get(), t.name.c_str()))
                    {
                        LOG_ERROR(DETECT_LOG_ID,
                                  "{} has no input tensor '{}' — re-export with canonical names",
                                  spec.onnxPath, t.name);
                        return false;
                    }
                }
                for(const TensorSpec& t : spec.outputs)
                {
                    if(!engineHasIOTensor(m_engine.get(), t.name.c_str()))
                    {
                        LOG_ERROR(DETECT_LOG_ID,
                                  "{} has no output tensor '{}' — re-export with canonical names",
                                  spec.onnxPath, t.name);
                        return false;
                    }
                }

                m_context.reset(m_engine->createExecutionContext());
                if(!m_context) { LOG_ERROR(DETECT_LOG_ID, "createExecutionContext failed"); return false; }

                if(cudaStreamCreate(&m_stream) != cudaSuccess)
                {
                    LOG_ERROR(DETECT_LOG_ID, "cudaStreamCreate failed");
                    return false;
                }

                m_inputs.resize(spec.inputs.size());
                for(size_t i = 0; i < spec.inputs.size(); i++)
                {
                    const size_t bytes = spec.inputs[i].floats() * sizeof(float);
                    m_inputs[i].floats = spec.inputs[i].floats();
                    if(!m_inputs[i].device.allocate(bytes) || !m_inputs[i].host.allocate(bytes))
                    {
                        LOG_ERROR(DETECT_LOG_ID, "CUDA buffer allocation failed for input '{}'",
                                  spec.inputs[i].name);
                        return false;
                    }
                    if(!m_context->setTensorAddress(spec.inputs[i].name.c_str(),
                                                    m_inputs[i].device.ptr))
                    {
                        LOG_ERROR(DETECT_LOG_ID, "setTensorAddress failed for input '{}'",
                                  spec.inputs[i].name);
                        return false;
                    }
                }

                m_outputs.resize(spec.outputs.size());
                for(size_t i = 0; i < spec.outputs.size(); i++)
                {
                    const size_t bytes = spec.outputs[i].floats() * sizeof(float);
                    m_outputs[i].floats = spec.outputs[i].floats();
                    if(!m_outputs[i].device.allocate(bytes) || !m_outputs[i].host.allocate(bytes))
                    {
                        LOG_ERROR(DETECT_LOG_ID, "CUDA buffer allocation failed for output '{}'",
                                  spec.outputs[i].name);
                        return false;
                    }
                    if(!m_context->setTensorAddress(spec.outputs[i].name.c_str(),
                                                    m_outputs[i].device.ptr))
                    {
                        LOG_ERROR(DETECT_LOG_ID, "setTensorAddress failed for output '{}'",
                                  spec.outputs[i].name);
                        return false;
                    }
                }
                return true;
            }

            float* input(size_t i) override
            {
                return i < m_inputs.size() ? static_cast<float*>(m_inputs[i].host.ptr) : nullptr;
            }

            const float* output(size_t i) const override
            {
                return i < m_outputs.size()
                     ? static_cast<const float*>(m_outputs[i].host.ptr) : nullptr;
            }

            size_t inputFloats(size_t i)  const override
            { return i < m_inputs.size()  ? m_inputs[i].floats  : 0; }

            size_t outputFloats(size_t i) const override
            { return i < m_outputs.size() ? m_outputs[i].floats : 0; }

            bool submit() override
            {
                for(Binding& b : m_inputs)
                {
                    if(cudaMemcpyAsync(b.device.ptr, b.host.ptr, b.floats * sizeof(float),
                                       cudaMemcpyHostToDevice, m_stream) != cudaSuccess)
                    {
                        LOG_WARNING(DETECT_LOG_ID, "cudaMemcpyAsync H2D failed");
                        return false;
                    }
                }

                if(!m_context->enqueueV3(m_stream))
                {
                    LOG_WARNING(DETECT_LOG_ID, "enqueueV3 failed");
                    return false;
                }

                for(Binding& b : m_outputs)
                {
                    if(cudaMemcpyAsync(b.host.ptr, b.device.ptr, b.floats * sizeof(float),
                                       cudaMemcpyDeviceToHost, m_stream) != cudaSuccess)
                    {
                        LOG_WARNING(DETECT_LOG_ID, "cudaMemcpyAsync D2H failed");
                        return false;
                    }
                }
                return true;
            }

            bool wait() override
            {
                if(cudaStreamSynchronize(m_stream) != cudaSuccess)
                {
                    LOG_WARNING(DETECT_LOG_ID, "cudaStreamSynchronize failed");
                    return false;
                }
                return true;
            }

        private:
            struct Binding
            {
                CudaBuffer   device;
                PinnedBuffer host;
                size_t       floats = 0;
            };

            std::unique_ptr<nvinfer1::ICudaEngine>       m_engine;
            std::unique_ptr<nvinfer1::IExecutionContext> m_context;
            cudaStream_t                                 m_stream = nullptr;
            std::vector<Binding>                         m_inputs;
            std::vector<Binding>                         m_outputs;
    };


    // ------------------------------------------------------------------
    // Backend
    // ------------------------------------------------------------------
    class TrtBackend : public InferenceBackend
    {
        public:
            Status init() override
            {
                m_runtime.reset(nvinfer1::createInferRuntime(g_trtLogger));
                if(!m_runtime)
                {
                    LOG_ERROR(DETECT_LOG_ID, "createInferRuntime failed");
                    return STATUS_ERROR_GENERIC;
                }
                return STATUS_OK;
            }

            std::unique_ptr<Model> load(const ModelSpec& spec,
                                        const ProgressFn& onProgress,
                                        const std::atomic<bool>& abort) override
            {
                if(!m_runtime) return nullptr;

                std::vector<uint8_t> plan;
                bool fromCache = false;
                if(!spec.cachePath.empty()
                && cachedEngineIsFresh(spec.cachePath, spec.onnxPath)
                && readFileBytes(spec.cachePath, plan))
                {
                    LOG_INFO(DETECT_LOG_ID, "trt backend: loaded cached engine from {} ({} bytes)",
                             spec.cachePath, plan.size());
                    fromCache = true;
                }

                if(fromCache)
                {
                    // Try the cache first, but fall through to a rebuild if it
                    // won't deserialize (GPU / TRT version mismatch).
                    auto model = std::make_unique<TrtModel>();
                    if(model->create(*m_runtime, plan, spec)) return model;

                    LOG_WARNING(DETECT_LOG_ID,
                                "Cached engine {} failed to load "
                                "(likely GPU/TRT version mismatch) — rebuilding",
                                spec.cachePath);
                    plan.clear();
                }

                if(abort.load(std::memory_order_acquire)) return nullptr;

                TrtProgressMonitor monitor(onProgress, abort);
                plan = buildEngineFromOnnx(spec.onnxPath, &monitor,
                                           spec.preferFp16, spec.fp32Layers);
                if(plan.empty() || abort.load(std::memory_order_acquire)) return nullptr;

                if(!spec.cachePath.empty())
                {
                    std::ofstream f(spec.cachePath, std::ios::binary);
                    if(f.write(reinterpret_cast<const char*>(plan.data()),
                               static_cast<std::streamsize>(plan.size())))
                    {
                        LOG_INFO(DETECT_LOG_ID, "trt backend: wrote engine cache to {}",
                                 spec.cachePath);
                    }
                    else
                    {
                        LOG_WARNING(DETECT_LOG_ID,
                                    "trt backend: failed to write engine cache to {} "
                                    "(errno={}); will rebuild next run",
                                    spec.cachePath, errno);
                    }
                }

                auto model = std::make_unique<TrtModel>();
                if(!model->create(*m_runtime, plan, spec))
                {
                    LOG_ERROR(DETECT_LOG_ID, "trt backend: freshly built plan failed to load");
                    return nullptr;
                }
                return model;
            }

            void shutdown() override { m_runtime.reset(); }

            const char* name() const override { return "TensorRT"; }

        private:
            std::unique_ptr<nvinfer1::IRuntime> m_runtime;
    };
}


InferenceBackendPtr createInferenceBackend()
{
    return std::make_unique<TrtBackend>();
}
