/**
 * inference_backend.hpp
 *
 * Abstract neural-network execution backend. DartDetector owns the dart
 * pipeline (preprocessing, conditioning mask, decode, state machine) and
 * knows nothing about how a forward pass is actually executed — that lives
 * behind this interface so the exact same pipeline runs against TensorRT on
 * the Jetson and against OpenCV DNN on a CPU-only machine.
 *
 * Exactly one implementation is compiled into any given binary, selected by
 * the TREBLOTRON_INFER_BACKEND CMake variable, and handed out by
 * createInferenceBackend().
 */

#ifndef DETECT_INFERENCE_BACKEND_HPP
#define DETECT_INFERENCE_BACKEND_HPP

#include "common_inc.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>


/** One model input or output, with the shape the pipeline expects. */
struct TensorSpec
{
    std::string      name;
    std::vector<int> shape;   // e.g. {1, 10, 720, 720}

    /** Total element count implied by `shape`. */
    size_t floats() const
    {
        size_t n = 1;
        for(int d : shape) n *= static_cast<size_t>(d);
        return n;
    }
};


class InferenceBackend
{
    public:
        /**
         * Progress reporting during a (potentially multi-minute) model load.
         * Arguments are fractional progress in [0, 1], a raw step counter for
         * "still doing something" feedback, and a short phase label.
         */
        using ProgressFn = std::function<void(float, uint64_t, const std::string&)>;

        struct ModelSpec
        {
            std::string onnxPath;

            /**
             * Where a compiled/serialized form of the model may be cached.
             * Rebuilt whenever the ONNX is newer. Backends with no compile
             * step (CPU) ignore this.
             */
            std::string cachePath;

            /** Backends without mixed precision ignore both of these. */
            bool                     preferFp16 = true;
            std::vector<std::string> fp32Layers;   // exact ONNX node names

            std::vector<TensorSpec> inputs;
            std::vector<TensorSpec> outputs;
        };

        /**
         * A loaded, ready-to-execute model with its I/O buffers allocated.
         *
         * Execution is split into submit()/wait() rather than a single
         * blocking call so a backend that can overlap independent models
         * (TensorRT, via one stream per model) actually does. The dart
         * pipeline submits the heavy U-Net and the palm detector together,
         * then waits on both.
         */
        class Model
        {
            public:
                virtual ~Model() = default;

                /** Writable host buffer for input `i`, sized per its TensorSpec. */
                virtual float* input(size_t i) = 0;

                /** Host-visible output buffer for output `i`. Valid after wait(). */
                virtual const float* output(size_t i) const = 0;

                virtual size_t inputFloats(size_t i)  const = 0;
                virtual size_t outputFloats(size_t i) const = 0;

                /** Begin execution. Inputs must already be written. */
                virtual bool submit() = 0;

                /** Block until outputs are host-visible. */
                virtual bool wait() = 0;

                bool run() { return submit() && wait(); }
        };

        virtual ~InferenceBackend() = default;

        /** Bring the backend up (streams, thread counts, …). */
        virtual Status init() = 0;

        /**
         * Load one model. Blocking, and potentially very slow on first run
         * (TensorRT compiles the engine). Returns nullptr on failure.
         * `abort` is polled throughout so shutdown during a build unwinds
         * promptly.
         */
        virtual std::unique_ptr<Model> load(const ModelSpec& spec,
                                            const ProgressFn& onProgress,
                                            const std::atomic<bool>& abort) = 0;

        virtual void shutdown() = 0;

        /** Short name for logs / the loading screen, e.g. "TensorRT", "CPU". */
        virtual const char* name() const = 0;
};

using InferenceBackendPtr = std::unique_ptr<InferenceBackend>;

/**
 * Create the backend this binary was built with. Returns nullptr when
 * TREBLOTRON_INFER_BACKEND=none, in which case DartDetector is unavailable
 * and callers must not construct one.
 */
InferenceBackendPtr createInferenceBackend();

#endif // DETECT_INFERENCE_BACKEND_HPP
