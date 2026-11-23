#pragma once
#include "detect/wire_calibration.hpp"
#include <opencv2/core.hpp>
#include <array>
#include <filesystem>
#include <vector>

struct CaptureCalibration
{
    std::array<double,9> homography{};
    std::vector<cv::Point2f> points;
    bool calibrated = false;
};
using CameraImages = std::array<cv::Mat, EXPECTED_CAMERA_COUNT>;
using CameraTransforms = std::array<CaptureCalibration, EXPECTED_CAMERA_COUNT>;

struct InferenceCapture
{
    CameraImages images;
    CameraTransforms transforms;
};

CameraTransforms captureCameraTransforms();
// Returns the capture stem, or an error description. JSON is published last.
std::string saveInferenceCapture(const CameraImages& frames, const CameraTransforms& transforms,
                                 const std::filesystem::path& directory, uint32_t sequence,
                                 bool missedDart, bool& ok);
