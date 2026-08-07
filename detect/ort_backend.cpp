/**
 * ort_backend.cpp
 *
 * ONNX Runtime inference backend with the DirectML execution provider.
 *
 * DirectML runs on any DirectX 12 GPU, which is what makes this the usable GPU
 * path on AMD and Intel hardware — TensorRT and CUDA are NVIDIA-only, and
 * OpenCV's OpenCL DNN backend measured 3-7x *slower* than its CPU path on a
 * Radeon RX 7900 XT.
 *
 * Measured on that card (RX 7900 XT, gfx1100), median over 8 runs:
 *
 *     model              OpenCV CPU   ORT CPU   ORT DirectML
 *     multicam_unet_ar      141 ms     52.7 ms       4.6 ms
 *     dart_seg_unet         111 ms     45.3 ms       4.1 ms
 *     blazepalm              15 ms      2.2 ms       0.9 ms
 *     hand_landmark          10 ms      0.6 ms       0.8 ms
 *
 * Outputs agree with the CPU provider to ~3e-4, well inside the thresholds the
 * decode uses.
 *
 * ONNX Runtime ships as MSVC-built binaries, which would normally be an ABI
 * problem for this MinGW build. It isn't, because the library is reached
 * entirely through its C API: one exported function, OrtGetApiBase, resolved
 * with GetProcAddress, returning a struct of function pointers. Nothing is
 * linked at build time, so a missing DLL is a clear runtime error rather than
 * a link failure.
 *
 * Compiled only when DARTLENS_INFER_BACKEND=directml. ModelSpec::cachePath,
 * ::preferFp16 and ::fp32Layers are ignored — ORT has no ahead-of-time compile
 * step and DirectML picks its own precision.
 */

#include "detect/inference_backend.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "onnxruntime_c_api.h"

#include <algorithm>
#include <string>
#include <vector>


namespace
{
    /** Resolved once, on first use, and shared by every model. */
    struct OrtLibrary
    {
        HMODULE       dll = nullptr;
        const OrtApi* api = nullptr;
        OrtStatus* (ORT_API_CALL* appendDml)(OrtSessionOptions*, int) = nullptr;

        bool valid() const { return api != nullptr; }
    };

    OrtLibrary g_ort;


    /** Human-readable text for an OrtStatus, which is then released. */
    std::string consumeStatus(OrtStatus* status)
    {
        if(!status) return {};
        std::string msg = g_ort.api->GetErrorMessage(status);
        g_ort.api->ReleaseStatus(status);
        return msg;
    }


    /** Log and swallow a failed call. Returns true when `status` was an error. */
    bool failed(OrtStatus* status, const char* what)
    {
        const std::string msg = consumeStatus(status);
        if(msg.empty()) return false;
        LOG_ERROR(DETECT_LOG_ID, "ort backend: {} failed — {}", what, msg);
        return true;
    }


    /**
     * ONNX Runtime takes wide paths on Windows (ORTCHAR_T is wchar_t). Model
     * paths here are ASCII by construction, so a widening copy is enough and
     * avoids dragging in a locale-dependent conversion.
     */
    std::wstring widen(const std::string& s)
    {
        return std::wstring(s.begin(), s.end());
    }


    /**
     * Log where DirectML.dll was actually resolved from, once.
     *
     * There are two possibilities and they behave differently: the copy
     * Windows ships in System32 (whose version tracks the OS, and on an older
     * Windows 10 may be too old for this ONNX Runtime) or a redistributable
     * shipped next to the executable. When a machine unexpectedly runs slowly,
     * this line is the first thing worth checking.
     */
    void reportDirectMlOrigin()
    {
        static bool reported = false;
        if(reported) return;
        reported = true;

        // Already loaded as a dependency of the DML provider — we only want to
        // look it up, never to pull in another copy.
        HMODULE dml = GetModuleHandleA("DirectML.dll");
        if(!dml)
        {
            LOG_WARNING(DETECT_LOG_ID, "ort backend: DirectML active but its module was not found");
            return;
        }

        char path[MAX_PATH] = {};
        if(GetModuleFileNameA(dml, path, MAX_PATH))
        {
            LOG_INFO(DETECT_LOG_ID, "ort backend: DirectML loaded from {}", path);
        }
    }


    bool loadOrtLibrary()
    {
        if(g_ort.dll) return g_ort.valid();

        // Plain name: the loader checks the executable's directory first, which
        // is where the build drops onnxruntime.dll. DirectML.dll resolves the
        // same way, falling through to the copy Windows ships in System32.
        g_ort.dll = LoadLibraryA("onnxruntime.dll");
        if(!g_ort.dll)
        {
            LOG_ERROR(DETECT_LOG_ID,
                      "ort backend: could not load onnxruntime.dll (error {}). It should sit "
                      "next to the executable — re-run cmake to fetch it.",
                      static_cast<unsigned long>(GetLastError()));
            return false;
        }

        using GetApiBaseFn = const OrtApiBase*(ORT_API_CALL*)();
        auto getApiBase = reinterpret_cast<GetApiBaseFn>(
            reinterpret_cast<void*>(GetProcAddress(g_ort.dll, "OrtGetApiBase")));
        if(!getApiBase)
        {
            LOG_ERROR(DETECT_LOG_ID, "ort backend: onnxruntime.dll exports no OrtGetApiBase");
            return false;
        }

        const OrtApiBase* base = getApiBase();
        if(!base) { LOG_ERROR(DETECT_LOG_ID, "ort backend: OrtGetApiBase returned null"); return false; }

        g_ort.api = base->GetApi(ORT_API_VERSION);
        if(!g_ort.api)
        {
            // The headers we built against are newer than the DLL. Say so
            // explicitly — the alternative is a null dereference later.
            LOG_ERROR(DETECT_LOG_ID,
                      "ort backend: onnxruntime.dll is too old for API version {} "
                      "(it reports '{}'). Delete external_libs/onnxruntime and re-run cmake.",
                      ORT_API_VERSION, base->GetVersionString());
            return false;
        }

        g_ort.appendDml = reinterpret_cast<OrtStatus*(ORT_API_CALL*)(OrtSessionOptions*, int)>(
            reinterpret_cast<void*>(
                GetProcAddress(g_ort.dll, "OrtSessionOptionsAppendExecutionProvider_DML")));
        if(!g_ort.appendDml)
        {
            LOG_WARNING(DETECT_LOG_ID,
                        "ort backend: this onnxruntime.dll has no DirectML provider — "
                        "falling back to CPU execution");
        }

        LOG_INFO(DETECT_LOG_ID, "ort backend: loaded ONNX Runtime {}", base->GetVersionString());
        return true;
    }


    // ------------------------------------------------------------------
    // Model
    // ------------------------------------------------------------------
    class OrtModel : public InferenceBackend::Model
    {
        public:
            ~OrtModel() override
            {
                const OrtApi* api = g_ort.api;
                if(!api) return;
                for(OrtValue* v : m_inputValues)  if(v) api->ReleaseValue(v);
                for(OrtValue* v : m_outputValues) if(v) api->ReleaseValue(v);
                if(m_memInfo) api->ReleaseMemoryInfo(m_memInfo);
                if(m_session) api->ReleaseSession(m_session);
            }

            bool create(OrtEnv* env, const InferenceBackend::ModelSpec& spec, bool useDml)
            {
                const OrtApi* api = g_ort.api;

                OrtSessionOptions* opts = nullptr;
                if(failed(api->CreateSessionOptions(&opts), "CreateSessionOptions")) return false;

                if(useDml)
                {
                    // DirectML requires both of these: it does not support the
                    // memory-pattern optimizer, and it must run nodes in a
                    // single sequential stream. ORT documents this as mandatory
                    // for the DML EP, and silently misbehaves without it.
                    failed(api->DisableMemPattern(opts), "DisableMemPattern");
                    failed(api->SetSessionExecutionMode(opts, ORT_SEQUENTIAL),
                           "SetSessionExecutionMode");

                    if(OrtStatus* s = g_ort.appendDml(opts, /*device_id*/ 0))
                    {
                        LOG_WARNING(DETECT_LOG_ID,
                                    "ort backend: DirectML unavailable ({}) — this model will "
                                    "run on the CPU", consumeStatus(s));
                    }
                    else
                    {
                        m_usingDml = true;
                        reportDirectMlOrigin();
                    }
                }

                api->SetSessionGraphOptimizationLevel(opts, ORT_ENABLE_ALL);

                const std::wstring wpath = widen(spec.onnxPath);
                OrtStatus* st = api->CreateSession(env, wpath.c_str(), opts, &m_session);
                api->ReleaseSessionOptions(opts);
                if(failed(st, "CreateSession")) return false;

                if(failed(api->CreateCpuMemoryInfo(OrtDeviceAllocator, OrtMemTypeDefault, &m_memInfo),
                          "CreateCpuMemoryInfo"))
                {
                    return false;
                }

                // Host-side buffers the pipeline writes into and reads out of.
                // Binding pre-allocated CPU tensors for the outputs too means
                // output(i) is a stable pointer across runs, matching what the
                // other backends hand out. ORT stages to and from the GPU
                // internally.
                if(!bind(spec.inputs,  m_inputStorage,  m_inputValues,  m_inputNames))  return false;
                if(!bind(spec.outputs, m_outputStorage, m_outputValues, m_outputNames)) return false;

                m_inputNamePtrs.clear();
                for(const std::string& n : m_inputNames)  m_inputNamePtrs.push_back(n.c_str());
                m_outputNamePtrs.clear();
                for(const std::string& n : m_outputNames) m_outputNamePtrs.push_back(n.c_str());

                if(!validateGraphNames(spec)) return false;

                LOG_INFO(DETECT_LOG_ID, "ort backend: loaded {} on {}",
                         spec.onnxPath, m_usingDml ? "DirectML" : "CPU");
                return true;
            }

            float* input(size_t i) override
            {
                return i < m_inputStorage.size() ? m_inputStorage[i].data() : nullptr;
            }

            const float* output(size_t i) const override
            {
                return i < m_outputStorage.size() ? m_outputStorage[i].data() : nullptr;
            }

            size_t inputFloats(size_t i) const override
            {
                return i < m_inputStorage.size() ? m_inputStorage[i].size() : 0;
            }

            size_t outputFloats(size_t i) const override
            {
                return i < m_outputStorage.size() ? m_outputStorage[i].size() : 0;
            }

            // ORT's Run is synchronous, so the work happens here and wait() is
            // a no-op. There is nothing to overlap: DirectML already keeps the
            // GPU busy for the duration of a single graph.
            bool submit() override
            {
                OrtStatus* st = g_ort.api->Run(
                    m_session, nullptr,
                    m_inputNamePtrs.data(), m_inputValues.data(), m_inputValues.size(),
                    m_outputNamePtrs.data(), m_outputNamePtrs.size(), m_outputValues.data());
                return !failed(st, "Run");
            }

            bool wait() override { return true; }

            /** False when this model fell back to CPU despite DirectML being asked for. */
            bool usingDml() const { return m_usingDml; }

        private:
            bool bind(const std::vector<TensorSpec>& specs,
                      std::vector<std::vector<float>>& storage,
                      std::vector<OrtValue*>& values,
                      std::vector<std::string>& names)
            {
                const OrtApi* api = g_ort.api;
                storage.resize(specs.size());
                values.assign(specs.size(), nullptr);
                names.clear();

                for(size_t i = 0; i < specs.size(); i++)
                {
                    const TensorSpec& t = specs[i];
                    names.push_back(t.name);
                    storage[i].assign(t.floats(), 0.0f);

                    std::vector<int64_t> shape(t.shape.begin(), t.shape.end());
                    OrtStatus* st = api->CreateTensorWithDataAsOrtValue(
                        m_memInfo, storage[i].data(), storage[i].size() * sizeof(float),
                        shape.data(), shape.size(),
                        ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &values[i]);
                    if(failed(st, "CreateTensorWithDataAsOrtValue")) return false;
                }
                return true;
            }

            /**
             * Assert the graph really exposes the names the pipeline hardcodes.
             * ORT's Run() reports an unknown name as a generic invalid-argument
             * error, so checking here keeps a renamed export as obvious as the
             * other backends make it.
             */
            bool validateGraphNames(const InferenceBackend::ModelSpec& spec)
            {
                const OrtApi* api = g_ort.api;
                OrtAllocator* alloc = nullptr;
                if(failed(api->GetAllocatorWithDefaultOptions(&alloc), "GetAllocatorWithDefaultOptions"))
                {
                    return false;
                }

                auto collect = [&](bool inputs, std::vector<std::string>& out) -> bool
                {
                    size_t count = 0;
                    OrtStatus* st = inputs ? api->SessionGetInputCount(m_session, &count)
                                           : api->SessionGetOutputCount(m_session, &count);
                    if(failed(st, "SessionGet*Count")) return false;
                    for(size_t i = 0; i < count; i++)
                    {
                        char* name = nullptr;
                        st = inputs ? api->SessionGetInputName(m_session, i, alloc, &name)
                                    : api->SessionGetOutputName(m_session, i, alloc, &name);
                        if(failed(st, "SessionGet*Name")) return false;
                        out.emplace_back(name);
                        alloc->Free(alloc, name);
                    }
                    return true;
                };

                std::vector<std::string> graphInputs, graphOutputs;
                if(!collect(true, graphInputs) || !collect(false, graphOutputs)) return false;

                auto check = [&](const std::vector<TensorSpec>& want,
                                 const std::vector<std::string>& have,
                                 const char* kind) -> bool
                {
                    for(const TensorSpec& t : want)
                    {
                        if(std::find(have.begin(), have.end(), t.name) != have.end()) continue;
                        std::string listed;
                        for(const std::string& h : have) { listed += " "; listed += h; }
                        LOG_ERROR(DETECT_LOG_ID,
                                  "ort backend: {} has no {} '{}' — re-export with canonical "
                                  "names. Available:{}", spec.onnxPath, kind, t.name, listed);
                        return false;
                    }
                    return true;
                };

                return check(spec.inputs, graphInputs, "input")
                    && check(spec.outputs, graphOutputs, "output");
            }

            OrtSession*     m_session = nullptr;
            OrtMemoryInfo*  m_memInfo = nullptr;
            bool            m_usingDml = false;

            std::vector<std::vector<float>> m_inputStorage;
            std::vector<std::vector<float>> m_outputStorage;
            std::vector<OrtValue*>          m_inputValues;
            std::vector<OrtValue*>          m_outputValues;
            std::vector<std::string>        m_inputNames;
            std::vector<std::string>        m_outputNames;
            std::vector<const char*>        m_inputNamePtrs;
            std::vector<const char*>        m_outputNamePtrs;
    };


    // ------------------------------------------------------------------
    // Backend
    // ------------------------------------------------------------------
    class OrtBackend : public InferenceBackend
    {
        public:
            ~OrtBackend() override { shutdown(); }

            Status init() override
            {
                if(!loadOrtLibrary()) return STATUS_ERROR_GENERIC;

                if(failed(g_ort.api->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "DartLens", &m_env),
                          "CreateEnv"))
                {
                    return STATUS_ERROR_GENERIC;
                }
                return STATUS_OK;
            }

            std::unique_ptr<Model> load(const ModelSpec& spec,
                                        const ProgressFn& onProgress,
                                        const std::atomic<bool>& abort) override
            {
                if(!m_env) return nullptr;
                if(abort.load(std::memory_order_acquire)) return nullptr;
                if(onProgress) onProgress(0.0f, 0, "Loading " + spec.onnxPath);

                auto model = std::make_unique<OrtModel>();
                if(!model->create(m_env, spec, g_ort.appendDml != nullptr)) return nullptr;

                // name() has to tell the truth about where the work actually
                // landed, not just which backend was requested — "is this
                // really on the GPU?" is the whole reason to pick this backend,
                // and a silent CPU fallback would look identical otherwise.
                if(!model->usingDml()) m_anyCpuFallback = true;

                if(onProgress) onProgress(1.0f, 0, "Loaded " + spec.onnxPath);
                return model;
            }

            void shutdown() override
            {
                if(m_env && g_ort.api)
                {
                    g_ort.api->ReleaseEnv(m_env);
                    m_env = nullptr;
                }
            }

            const char* name() const override
            {
                if(!g_ort.appendDml) return "ONNX Runtime (CPU — no DirectML provider)";
                if(m_anyCpuFallback)  return "ONNX Runtime (CPU — DirectML unavailable)";
                return "DirectML (ONNX Runtime)";
            }

        private:
            OrtEnv* m_env = nullptr;
            bool    m_anyCpuFallback = false;
    };
}


InferenceBackendPtr createInferenceBackend()
{
    return std::make_unique<OrtBackend>();
}
