/**
 * wire_calibration.cpp
 *
 * Wire calibration storage, homography fitting, and frame warping.
 */

#include "detect/wire_calibration.hpp"

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/calib3d.hpp>

#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>


// ============================================================================
// Constants — match DartModelTraining template space
// ============================================================================

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// Center of the 720x720 destination image
static constexpr float TEMPLATE_CENTER = WARPED_OUTPUT_SIZE * 0.5f;

// Outer-double radius in pixels (matches BOARD_RADIUS_PX in board_detection.py)
static constexpr float BOARD_RADIUS_PX = 290.0f;

// Ring radius ratios from board_detection.py
static constexpr float RING_RATIO_OUTER_TRIPLE = 0.629f;
static constexpr float RING_RATIO_OUTER_DOUBLE = 1.000f;

// Wire angles (OpenCV convention: 0=right, +CW since y points down)
static constexpr float WIRE_START_ANGLE_DEG = 279.0f; // 20/1 wire
static constexpr float SEGMENT_ANGLE_DEG    = 18.0f;

static constexpr uint32_t POINTS_PER_RING = 20;


// ============================================================================
// Per-camera state
// ============================================================================

struct WireCalibrationSlot
{
    std::vector<cv::Point2f> points;     // up to 40 clicked points
    cv::Mat                  homography;

    // Pre-baked remap tables — cv::remap with fixed-point maps is dramatically
    // faster than cv::warpPerspective on ARM because the per-pixel inverse
    // homography evaluation + interpolation-weight compute happens once at
    // calibration time instead of every frame. map1 is CV_16SC2 (integer src
    // coordinates), map2 is CV_16UC1 (interpolation weights).
    cv::Mat                  remapXY;    // CV_16SC2, 720x720
    cv::Mat                  remapFrac;  // CV_16UC1, 720x720

    bool                     calibrated = false;
};

static std::array<WireCalibrationSlot, EXPECTED_CAMERA_COUNT> f_slots;
static std::array<cv::Point2f, WIRE_POINTS_PER_CAMERA> f_templatePoints;
static bool f_initialized = false;


// ============================================================================
// Template point computation
// ============================================================================

static void computeTemplatePoints()
{
    // Two rings (triple, double), 20 points each, clockwise from 20/1 wire.
    // Order in the template array: [triple 0..19, double 0..19].
    const float ringRadii[2] = {
        BOARD_RADIUS_PX * RING_RATIO_OUTER_TRIPLE,
        BOARD_RADIUS_PX * RING_RATIO_OUTER_DOUBLE
    };

    for(uint32_t ring = 0; ring < 2; ring++)
    {
        for(uint32_t i = 0; i < POINTS_PER_RING; i++)
        {
            float angleDeg = WIRE_START_ANGLE_DEG + static_cast<float>(i) * SEGMENT_ANGLE_DEG;
            float angleRad = angleDeg * static_cast<float>(M_PI) / 180.0f;
            float x = TEMPLATE_CENTER + ringRadii[ring] * std::cos(angleRad);
            float y = TEMPLATE_CENTER + ringRadii[ring] * std::sin(angleRad);
            f_templatePoints[ring * POINTS_PER_RING + i] = {x, y};
        }
    }
}


// ============================================================================
// Lifecycle
// ============================================================================

void initializeWireCalibration()
{
    if(f_initialized)
    {
        return;
    }

    for(auto& slot : f_slots)
    {
        slot.points.clear();
        slot.points.reserve(WIRE_POINTS_PER_CAMERA);
        slot.homography.release();
        slot.remapXY.release();
        slot.remapFrac.release();
        slot.calibrated = false;
    }

    computeTemplatePoints();
    f_initialized = true;

    LOG_INFO(DETECT_LOG_ID, "Wire calibration initialized");
}


void shutdownWireCalibration()
{
    for(auto& slot : f_slots)
    {
        slot.points.clear();
        slot.homography.release();
        slot.remapXY.release();
        slot.remapFrac.release();
        slot.calibrated = false;
    }
    f_initialized = false;
}


// ============================================================================
// Homography fitting
// ============================================================================

static void recomputeHomography(uint32_t camIndex)
{
    WireCalibrationSlot& slot = f_slots[camIndex];

    if(slot.points.size() != WIRE_POINTS_PER_CAMERA)
    {
        slot.homography.release();
        slot.calibrated = false;
        return;
    }

    std::vector<cv::Point2f> dst(f_templatePoints.begin(), f_templatePoints.end());

    slot.homography = cv::findHomography(slot.points, dst, cv::RANSAC, 3.0);
    if(slot.homography.empty())
    {
        slot.calibrated = false;
        LOG_WARNING(DETECT_LOG_ID, "findHomography failed for camera {}", camIndex);
        return;
    }

    // Bake the inverse homography into a pair of dense 720x720 remap tables.
    // For each destination pixel (u, v) we compute the source coordinate
    //   [sx, sy, w] = H_inv * [u, v, 1]
    //   (sx, sy) /= w
    // then convertMaps() packs them into the INTER_LINEAR fixed-point format
    // that cv::remap's NEON path accelerates. This moves the expensive
    // per-pixel perspective math out of the capture hot loop entirely.
    cv::Mat Hinv = slot.homography.inv();
    const double* H = Hinv.ptr<double>(0);

    const int N = static_cast<int>(WARPED_OUTPUT_SIZE);
    cv::Mat mapX(N, N, CV_32FC1);
    cv::Mat mapY(N, N, CV_32FC1);
    for(int v = 0; v < N; v++)
    {
        float* rowX = mapX.ptr<float>(v);
        float* rowY = mapY.ptr<float>(v);
        for(int u = 0; u < N; u++)
        {
            double w = H[6] * u + H[7] * v + H[8];
            double sx = (H[0] * u + H[1] * v + H[2]) / w;
            double sy = (H[3] * u + H[4] * v + H[5]) / w;
            rowX[u] = static_cast<float>(sx);
            rowY[u] = static_cast<float>(sy);
        }
    }
    cv::convertMaps(mapX, mapY, slot.remapXY, slot.remapFrac, CV_16SC2);

    slot.calibrated = true;
    LOG_INFO(DETECT_LOG_ID, "Camera {} calibrated ({} points, remap tables baked)",
             camIndex, slot.points.size());
}


// ============================================================================
// Point management
// ============================================================================

uint32_t getWirePointCount(uint32_t camIndex)
{
    if(camIndex >= EXPECTED_CAMERA_COUNT) return 0;
    return static_cast<uint32_t>(f_slots[camIndex].points.size());
}


bool isCameraCalibrated(uint32_t camIndex)
{
    if(camIndex >= EXPECTED_CAMERA_COUNT) return false;
    return f_slots[camIndex].calibrated;
}


void addWirePoint(uint32_t camIndex, float srcX, float srcY)
{
    if(camIndex >= EXPECTED_CAMERA_COUNT) return;

    WireCalibrationSlot& slot = f_slots[camIndex];
    if(slot.points.size() >= WIRE_POINTS_PER_CAMERA)
    {
        return;
    }

    slot.points.push_back({srcX, srcY});

    if(slot.points.size() == WIRE_POINTS_PER_CAMERA)
    {
        recomputeHomography(camIndex);
    }
}


void undoLastWirePoint(uint32_t camIndex)
{
    if(camIndex >= EXPECTED_CAMERA_COUNT) return;

    WireCalibrationSlot& slot = f_slots[camIndex];
    if(!slot.points.empty())
    {
        slot.points.pop_back();
        slot.homography.release();
        slot.remapXY.release();
        slot.remapFrac.release();
        slot.calibrated = false;
    }
}


void clearWirePoints(uint32_t camIndex)
{
    if(camIndex >= EXPECTED_CAMERA_COUNT) return;

    WireCalibrationSlot& slot = f_slots[camIndex];
    slot.points.clear();
    slot.homography.release();
    slot.remapXY.release();
    slot.remapFrac.release();
    slot.calibrated = false;
}


bool getWirePoint(uint32_t camIndex, uint32_t pointIndex, float& outX, float& outY)
{
    if(camIndex >= EXPECTED_CAMERA_COUNT) return false;

    const WireCalibrationSlot& slot = f_slots[camIndex];
    if(pointIndex >= slot.points.size())
    {
        return false;
    }

    outX = slot.points[pointIndex].x;
    outY = slot.points[pointIndex].y;
    return true;
}


std::string getNextPointLabel(uint32_t camIndex)
{
    if(camIndex >= EXPECTED_CAMERA_COUNT) return "";

    uint32_t count = getWirePointCount(camIndex);
    if(count >= WIRE_POINTS_PER_CAMERA)
    {
        return "";
    }

    const char* ringName = (count < POINTS_PER_RING) ? "Triple" : "Double";
    uint32_t indexInRing = (count % POINTS_PER_RING) + 1;

    char buf[32];
    std::snprintf(buf, sizeof(buf), "%s %u/%u", ringName, indexInRing, POINTS_PER_RING);
    return std::string(buf);
}


// ============================================================================
// Warping
// ============================================================================

bool warpCameraFrame(uint32_t camIndex, const cv::Mat& src, cv::Mat& dst)
{
    if(camIndex >= EXPECTED_CAMERA_COUNT) return false;

    const WireCalibrationSlot& slot = f_slots[camIndex];
    if(!slot.calibrated || slot.remapXY.empty() || src.empty())
    {
        return false;
    }

    cv::remap(src, dst, slot.remapXY, slot.remapFrac,
              cv::INTER_LINEAR, cv::BORDER_CONSTANT, cv::Scalar(0, 0, 0));
    return true;
}


bool warpCameraMask(uint32_t camIndex, const cv::Mat& src, cv::Mat& dst)
{
    if(camIndex >= EXPECTED_CAMERA_COUNT) return false;

    const WireCalibrationSlot& slot = f_slots[camIndex];
    if(!slot.calibrated || slot.remapXY.empty() || src.empty())
    {
        return false;
    }

    // INTER_NEAREST ignores the fractional table entirely, so the same baked
    // maps serve both variants and nothing extra has to be computed.
    cv::remap(src, dst, slot.remapXY, slot.remapFrac,
              cv::INTER_NEAREST, cv::BORDER_CONSTANT, cv::Scalar(0, 0, 0));
    return true;
}


bool getCameraHomography(uint32_t camIndex, double out[9])
{
    if(camIndex >= EXPECTED_CAMERA_COUNT || out == nullptr) return false;

    const WireCalibrationSlot& slot = f_slots[camIndex];
    if(!slot.calibrated || slot.homography.empty()) return false;

    // findHomography always returns a 3x3 CV_64F; copy element-wise rather than
    // memcpy'ing so a non-continuous Mat can never be read wrong.
    for(int r = 0; r < 3; r++)
    {
        for(int c = 0; c < 3; c++)
        {
            out[r * 3 + c] = slot.homography.at<double>(r, c);
        }
    }
    return true;
}


// ============================================================================
// Save / Load
// ============================================================================

Status saveWireCalibration(const std::string& path)
{
    std::error_code ec;
    std::filesystem::path p(path);
    if(p.has_parent_path())
    {
        std::filesystem::create_directories(p.parent_path(), ec);
        if(ec)
        {
            LOG_ERROR(DETECT_LOG_ID, "Failed to create config directory: {}", ec.message());
            return STATUS_ERROR_GENERIC;
        }
    }

    std::ofstream out(path, std::ios::trunc);
    if(!out)
    {
        LOG_ERROR(DETECT_LOG_ID, "Failed to open {} for writing", path);
        return STATUS_ERROR_GENERIC;
    }

    out << "# wire_calibration.txt\n";
    out << "# 40 points per camera: 20 outer-triple, 20 outer-double\n";

    for(uint32_t c = 0; c < EXPECTED_CAMERA_COUNT; c++)
    {
        const WireCalibrationSlot& slot = f_slots[c];
        if(slot.points.size() != WIRE_POINTS_PER_CAMERA)
        {
            continue;
        }

        out << "camera " << c << "\n";
        for(const auto& pt : slot.points)
        {
            out << pt.x << " " << pt.y << "\n";
        }
    }

    LOG_INFO(DETECT_LOG_ID, "Wire calibration saved to {}", path);
    return STATUS_OK;
}


Status loadWireCalibration(const std::string& path)
{
    std::ifstream in(path);
    if(!in)
    {
        LOG_INFO(DETECT_LOG_ID, "No wire calibration file at {}", path);
        return STATUS_ERROR_GENERIC;
    }

    int currentCam = -1;
    std::vector<cv::Point2f> buffer;
    auto commitBuffer = [&]()
    {
        if(currentCam >= 0 && currentCam < static_cast<int>(EXPECTED_CAMERA_COUNT))
        {
            if(buffer.size() == WIRE_POINTS_PER_CAMERA)
            {
                f_slots[currentCam].points = buffer;
                recomputeHomography(static_cast<uint32_t>(currentCam));
            }
            else
            {
                LOG_WARNING(DETECT_LOG_ID,
                            "Camera {} has {} points in file (expected {}), skipping",
                            currentCam, buffer.size(), WIRE_POINTS_PER_CAMERA);
            }
        }
        buffer.clear();
    };

    std::string line;
    while(std::getline(in, line))
    {
        if(line.empty() || line[0] == '#')
        {
            continue;
        }

        if(line.rfind("camera ", 0) == 0)
        {
            commitBuffer();
            currentCam = std::atoi(line.c_str() + 7);
            continue;
        }

        float x = 0, y = 0;
        if(std::sscanf(line.c_str(), "%f %f", &x, &y) == 2)
        {
            buffer.push_back({x, y});
        }
    }
    commitBuffer();

    LOG_INFO(DETECT_LOG_ID, "Wire calibration loaded from {}", path);
    return STATUS_OK;
}
