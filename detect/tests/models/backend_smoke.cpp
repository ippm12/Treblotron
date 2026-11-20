// Standalone native backend smoke test. See the model bundle README for build
// instructions; uses the production backend and its verified-engine selection.
#include "detect/inference_backend.hpp"
#include <spdlog/sinks/stdout_color_sinks.h>
#include <algorithm>
#include <cmath>
#include <iostream>

LoggerPtr getLogger(LogID)
{
    static auto logger = spdlog::stderr_color_mt("backend-smoke");
    return logger;
}

int main(int argc, char** argv)
{
    if(argc != 2) { std::cerr << "Usage: backend-smoke <regnetx_fast directory>\n"; return 2; }
    const std::string root = argv[1];
    auto backend = createInferenceBackend();
    if(!backend || IS_STATUS_NOT_OK(backend->init())) return 1;
    std::atomic<bool> abort{false};
    for(const std::string id : {"seg", "ar"})
    {
        InferenceBackend::ModelSpec spec;
        spec.onnxPath = root + (id == "seg" ? "/dart_seg_unet.onnx" : "/multicam_unet_ar_heatmap_only.onnx");
        spec.prebuiltPaths = {root + "/engines/" + id + ".int8.plan",
                              root + "/engines/" + id + ".fp16.plan"};
        spec.inputs = {{"input", id == "seg" ? std::vector<int>{3, 3, 360, 640}
                                               : std::vector<int>{1, 21, 720, 720}}};
        spec.outputs = {{id == "seg" ? "logits" : "heatmap",
                         id == "seg" ? std::vector<int>{3, 1, 360, 640}
                                     : std::vector<int>{1, 360, 360}}};
        auto model = backend->load(spec, {}, abort);
        if(!model) return 1;
        std::fill_n(model->input(0), model->inputFloats(0), 0.0f);
        for(int iteration = 0; iteration < 3; ++iteration)
        {
            if(!model->run()) return 1;
            for(size_t i = 0; i < model->outputFloats(0); ++i)
                if(!std::isfinite(model->output(0)[i])) return 1;
        }
        std::cout << "PASS " << id << ": production backend loaded and ran 3 passes\n";
    }
    backend->shutdown();
}
