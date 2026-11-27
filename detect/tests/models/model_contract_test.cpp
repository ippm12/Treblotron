#include "../../heatmap_peak.hpp"
#include "../../verified_engine.hpp"

#include <cassert>
#include <filesystem>
#include <iostream>

int main()
{
    const float threshold = DART_PEAK_LOGIT_THRESHOLD;
    float values[] = {-10.0f, std::nextafter(threshold, -INFINITY), -4.0f};
    assert(findDartHeatmapPeak(values, 3).valid());
    assert(!findDartHeatmapPeak(values, 3).present());
    values[1] = threshold;
    assert(findDartHeatmapPeak(values, 3).present());
    values[2] = threshold + 1;
    assert(findDartHeatmapPeak(values, 3).index == 2);
    values[1] = std::numeric_limits<float>::quiet_NaN();
    assert(!findDartHeatmapPeak(values, 3).present());
    assert(!findDartHeatmapPeak(values, 3).valid());
    assert(!findDartHeatmapPeak(nullptr, 0).valid());
    // Check the removed head's decisions over positive/negative real logits.
    for(int i = -2000; i <= 2000; ++i)
    {
        const float v = i / 100.0f;
        const bool old = 1.9002968072891235f * v + 1.693196415901184f >= 0
                      && 1.0f / (1.0f + std::exp(-v)) >= 0.55f;
        assert(old == (DartHeatmapPeak{0, v}.present()));
    }

    const std::vector<uint8_t> bytes = {'1','2','3','4','5','6','7','8','9'};
    assert(verified_engine::crc32(bytes) == 0xcbf43926u); // standard CRC32 vector
    const std::string source = "receipt-test.onnx", plan = "receipt-test.plan";
    std::ofstream(source, std::ios::binary) << "123456789";
    assert(!verified_engine::matches(plan, source, bytes));
    std::ofstream(plan + ".verified") << "TREBLOTRON_VERIFIED_ENGINE_V1\ncbf43926 cbf43926\n";
    assert(verified_engine::matches(plan, source, bytes));
    auto changed = bytes;
    changed.back() = '0';
    assert(!verified_engine::matches(plan, source, changed));
    std::ofstream(source, std::ios::binary) << "replacement model";
    assert(!verified_engine::matches(plan, source, bytes));
    std::filesystem::remove(source);
    std::filesystem::remove(plan + ".verified");
    std::cout << "PASS model contract: heatmap gate, invalid output, stale engine/model receipts\n";
}
