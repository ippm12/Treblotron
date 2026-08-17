/**
 * cpu_backend.cpp
 *
 * OpenCV DNN inference backend. Runs the ONNX graphs directly on the CPU with
 * no GPU, no vendor SDK, and no extra dependency beyond the OpenCV that the
 * project already vendors — which is what lets the inference server run on an
 * ordinary desktop.
 *
 * Compiled only when DARTMATIC_INFER_BACKEND=cpu. There is no compile/cache
 * step here, so ModelSpec::cachePath, ::preferFp16 and ::fp32Layers are
 * ignored; loading is just readNetFromONNX plus a shape validation pass.
 *
 * Measured on a Ryzen 7 9800X3D (16 threads): ~145 ms for multicam_unet_ar,
 * ~115 ms for dart_seg_unet, ~15 ms for BlazePalm, ~10 ms for hand_landmark.
 */

#include "detect/inference_backend.hpp"

#include <opencv2/core.hpp>
#include <opencv2/dnn.hpp>

#include <algorithm>
#include <cstring>
#include <thread>


namespace
{
    class CpuModel : public InferenceBackend::Model
    {
        public:
            CpuModel(cv::dnn::Net net,
                     const std::vector<TensorSpec>& inputs,
                     const std::vector<TensorSpec>& outputs)
                : m_net(std::move(net))
                , m_inputSpecs(inputs)
                , m_outputSpecs(outputs)
            {
                m_inputBlobs.reserve(inputs.size());
                for(const TensorSpec& t : inputs)
                {
                    m_inputBlobs.emplace_back(static_cast<int>(t.shape.size()),
                                              t.shape.data(), CV_32F);
                }

                m_outputNames.reserve(outputs.size());
                m_outputData.resize(outputs.size());
                for(size_t i = 0; i < outputs.size(); i++)
                {
                    m_outputNames.push_back(outputs[i].name);
                    m_outputData[i].assign(outputs[i].floats(), 0.0f);
                }
            }

            float* input(size_t i) override
            {
                return i < m_inputBlobs.size() ? m_inputBlobs[i].ptr<float>() : nullptr;
            }

            const float* output(size_t i) const override
            {
                return i < m_outputData.size() ? m_outputData[i].data() : nullptr;
            }

            size_t inputFloats(size_t i) const override
            {
                return i < m_inputSpecs.size() ? m_inputSpecs[i].floats() : 0;
            }

            size_t outputFloats(size_t i) const override
            {
                return i < m_outputSpecs.size() ? m_outputSpecs[i].floats() : 0;
            }

            // OpenCV DNN is synchronous, so all the work happens here and
            // wait() is a no-op. cv::dnn already parallelises across every
            // core internally, so there is nothing to gain from deferring.
            bool submit() override
            {
                try
                {
                    for(size_t i = 0; i < m_inputBlobs.size(); i++)
                    {
                        m_net.setInput(m_inputBlobs[i], m_inputSpecs[i].name);
                    }

                    m_net.forward(m_outputMats, m_outputNames);

                    if(m_outputMats.size() != m_outputData.size())
                    {
                        LOG_ERROR(DETECT_LOG_ID,
                                  "cpu backend: expected {} outputs, forward returned {}",
                                  m_outputData.size(), m_outputMats.size());
                        return false;
                    }

                    for(size_t i = 0; i < m_outputMats.size(); i++)
                    {
                        const cv::Mat& m = m_outputMats[i];
                        const size_t   n = m_outputData[i].size();
                        if(!m.isContinuous() || m.total() != n || m.type() != CV_32F)
                        {
                            LOG_ERROR(DETECT_LOG_ID,
                                      "cpu backend: output '{}' is {} floats (type {}), expected {}",
                                      m_outputSpecs[i].name, m.total(), m.type(), n);
                            return false;
                        }
                        std::memcpy(m_outputData[i].data(), m.ptr<float>(), n * sizeof(float));
                    }
                    return true;
                }
                catch(const cv::Exception& e)
                {
                    LOG_ERROR(DETECT_LOG_ID, "cpu backend forward failed: {}", e.what());
                    return false;
                }
            }

            bool wait() override { return true; }

        private:
            cv::dnn::Net             m_net;
            std::vector<TensorSpec>  m_inputSpecs;
            std::vector<TensorSpec>  m_outputSpecs;
            std::vector<cv::Mat>     m_inputBlobs;
            std::vector<std::string> m_outputNames;
            std::vector<cv::Mat>     m_outputMats;
            std::vector<std::vector<float>> m_outputData;
    };


    class CpuBackend : public InferenceBackend
    {
        public:
            Status init() override
            {
                const unsigned hw = std::thread::hardware_concurrency();
                const int threads = static_cast<int>(std::max(1u, hw));
                cv::setNumThreads(threads);
                LOG_INFO(DETECT_LOG_ID, "cpu backend: OpenCV DNN on {} threads", threads);
                return STATUS_OK;
            }

            std::unique_ptr<Model> load(const ModelSpec& spec,
                                        const ProgressFn& onProgress,
                                        const std::atomic<bool>& abort) override
            {
                if(abort.load(std::memory_order_acquire)) return nullptr;
                if(onProgress) onProgress(0.0f, 0, "Loading " + spec.onnxPath);

                cv::dnn::Net net;
                try
                {
                    net = cv::dnn::readNetFromONNX(spec.onnxPath);
                }
                catch(const cv::Exception& e)
                {
                    LOG_ERROR(DETECT_LOG_ID, "cpu backend: failed to read {}: {}",
                              spec.onnxPath, e.what());
                    return nullptr;
                }
                if(net.empty())
                {
                    LOG_ERROR(DETECT_LOG_ID, "cpu backend: {} loaded as an empty network",
                              spec.onnxPath);
                    return nullptr;
                }

                net.setPreferableBackend(cv::dnn::DNN_BACKEND_OPENCV);
                net.setPreferableTarget(cv::dnn::DNN_TARGET_CPU);

                // Assert the canonical output names are actually present.
                // cv::dnn::forward() on an unknown name throws deep inside the
                // graph walker with an unhelpful message, so check up front —
                // same contract the TensorRT backend enforces via
                // engineHasIOTensor.
                const std::vector<std::string> available = net.getUnconnectedOutLayersNames();
                for(const TensorSpec& t : spec.outputs)
                {
                    if(std::find(available.begin(), available.end(), t.name) == available.end())
                    {
                        std::string have;
                        for(const std::string& a : available) { have += " "; have += a; }
                        LOG_ERROR(DETECT_LOG_ID,
                                  "cpu backend: {} has no output '{}' — re-export with "
                                  "canonical names. Available:{}",
                                  spec.onnxPath, t.name, have);
                        return nullptr;
                    }
                }

                if(onProgress) onProgress(1.0f, 0, "Loaded " + spec.onnxPath);
                LOG_INFO(DETECT_LOG_ID, "cpu backend: loaded {}", spec.onnxPath);

                return std::make_unique<CpuModel>(std::move(net), spec.inputs, spec.outputs);
            }

            void shutdown() override {}

            const char* name() const override { return "CPU (OpenCV DNN)"; }
    };
}


InferenceBackendPtr createInferenceBackend()
{
    return std::make_unique<CpuBackend>();
}
