#pragma once

#include <array>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace verified_engine
{
// CRC32 identifies stale/replaced artifacts, not authenticity. Engines must
// still come from a trusted local build. Python uses zlib.crc32 for this format.
inline constexpr auto crcTable = [] {
    std::array<uint32_t, 256> table{};
    for(uint32_t n = 0; n < table.size(); ++n)
    {
        uint32_t value = n;
        for(int bit = 0; bit < 8; ++bit)
            value = (value >> 1) ^ ((value & 1) ? 0xedb88320u : 0u);
        table[n] = value;
    }
    return table;
}();

inline uint32_t crc32(const std::vector<uint8_t>& data)
{
    uint32_t value = 0xffffffffu;
    for(uint8_t byte : data) value = crcTable[(value ^ byte) & 255u] ^ (value >> 8);
    return value ^ 0xffffffffu;
}

inline bool fileCrc32(const std::string& path, uint32_t& result)
{
    std::ifstream file(path, std::ios::binary);
    if(!file) return false;
    uint32_t value = 0xffffffffu;
    std::array<char, 65536> buffer;
    while(file.read(buffer.data(), buffer.size()) || file.gcount())
        for(std::streamsize i = 0; i < file.gcount(); ++i)
            value = crcTable[(value ^ static_cast<uint8_t>(buffer[i])) & 255u] ^ (value >> 8);
    if(!file.eof()) return false;
    result = value ^ 0xffffffffu;
    return true;
}

inline bool matches(const std::string& planPath, const std::string& onnxPath,
                    const std::vector<uint8_t>& plan)
{
    std::ifstream receipt(planPath + ".verified");
    std::string version, extra;
    uint32_t onnxCrc = 0, planCrc = 0, actualOnnxCrc = 0;
    if(!(receipt >> version >> std::hex >> onnxCrc >> planCrc)
    || version != "TREBLOTRON_VERIFIED_ENGINE_V1" || (receipt >> extra)) return false;
    return !plan.empty() && fileCrc32(onnxPath, actualOnnxCrc)
        && onnxCrc == actualOnnxCrc && planCrc == crc32(plan);
}
}
