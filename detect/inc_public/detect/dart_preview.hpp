#pragma once
#include <cstdint>
#include <vector>
#include <memory>
struct InferenceCapture;

// One successful inference. All four planes describe this exact input set.
struct DartPreview
{
    static constexpr int cameraWidth = 320, cameraHeight = 180, boardSize = 360;
    static constexpr int cameraPixels = cameraWidth * cameraHeight;
    static constexpr int boardPixels = boardSize * boardSize;
    // Local-only ownership of the exact originals while this preview is displayed.
    std::shared_ptr<const InferenceCapture> original;
    uint32_t sequence = 0;
    uint8_t camera = 0;
    std::vector<uint8_t> rgb, maskBits, boardGray, heat;
    // Positions in the aligned board image, in pixel pairs.
    std::vector<uint16_t> tips;
};
