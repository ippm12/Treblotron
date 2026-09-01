/**
 * wire_calibration.hpp
 *
 * Per-camera dartboard wire calibration. The user clicks 40 wire intersection
 * points per camera (20 outer-triple-ring + 20 outer-double-ring, clockwise
 * from the 20/1 boundary wire). These are paired with template points in a
 * 720x720 destination space and used to compute a perspective homography
 * (cv::findHomography with RANSAC) that warps each camera frame into the
 * canonical dartboard view consumed by the dart detection model.
 */

#ifndef WIRE_CALIBRATION_HPP
#define WIRE_CALIBRATION_HPP

#include "common_inc.hpp"
#include <cstdint>
#include <string>

namespace cv { class Mat; }

/**
 * The system expects exactly 3 cameras. Lives here rather than in
 * vision/vision.hpp because the rig's camera count is a property of the
 * calibration geometry, and the detect module must not depend on vision —
 * the inference server links detect without any camera capture code at all.
 */
constexpr uint32_t EXPECTED_CAMERA_COUNT = 3;

// 20 outer-triple-ring + 20 outer-double-ring intersections
constexpr uint32_t WIRE_POINTS_PER_CAMERA = 40;

// Destination warped image size (matches the model input)
constexpr uint32_t WARPED_OUTPUT_SIZE = 720;

// Wire intersections per ring, i.e. one per bed boundary.
constexpr uint32_t WIRE_POINTS_PER_RING = 20;

/**
 * Template geometry, shared with the UI so the calibration screen can draw the
 * same board the homography is fitted to.
 *
 * Angles use the OpenCV convention: 0 deg points right and positive turns
 * clockwise, because y grows downwards. Point i of a ring therefore sits at
 * WIRE_START_ANGLE_DEG + i * WIRE_SEGMENT_ANGLE_DEG, starting at the wire
 * between beds 20 and 1 and working clockwise around the board.
 */
constexpr float WIRE_START_ANGLE_DEG    = 279.0f;
constexpr float WIRE_SEGMENT_ANGLE_DEG  = 18.0f;

/** Outer-triple radius as a fraction of the outer-double radius. */
constexpr float WIRE_TRIPLE_RADIUS_RATIO = 0.629f;

/**
 * Initialize the wire calibration module. Allocates per-camera state for
 * EXPECTED_CAMERA_COUNT cameras and computes the template (destination) points.
 */
void initializeWireCalibration();

/** Free all wire calibration state. */
void shutdownWireCalibration();

/**
 * Number of points placed for a given camera (0..WIRE_POINTS_PER_CAMERA).
 */
uint32_t getWirePointCount(uint32_t camIndex);

/** Returns true if all WIRE_POINTS_PER_CAMERA points have been placed. */
bool isCameraCalibrated(uint32_t camIndex);

/**
 * Append a clicked point (in source image pixel coordinates) for the given
 * camera. The next point's index will be getWirePointCount() before this call.
 * Recomputes the homography automatically when the count reaches 40.
 */
void addWirePoint(uint32_t camIndex, float srcX, float srcY);

/** Remove the most recently added point for the given camera. */
void undoLastWirePoint(uint32_t camIndex);

/** Clear all points for the given camera. */
void clearWirePoints(uint32_t camIndex);

/**
 * Read back a stored point. Returns false if pointIndex is out of range
 * for the camera's current point count.
 */
bool getWirePoint(uint32_t camIndex, uint32_t pointIndex, float& outX, float& outY);

/**
 * Where a given point index belongs on the board.
 *
 * Every point sits on the *outer* edge of its ring — the boundary the ring
 * shares with the single bed outside it, not the inner edge and not the middle
 * of the ring — at the wire that divides two numbered beds.
 */
struct WireTarget
{
    bool     tripleRing    = true;  // true: outer triple edge. false: outer double edge.
    uint32_t wireIndex     = 0;     // 0..19, clockwise from the 20/1 wire
    uint8_t  sectionBefore = 20;    // bed counter-clockwise of the wire (20 for wire 0)
    uint8_t  sectionAfter  = 1;     // bed clockwise of the wire (1 for wire 0)
    float    angleDeg      = WIRE_START_ANGLE_DEG;  // template angle, OpenCV convention
};

/**
 * Describe the point at `pointIndex` (0..WIRE_POINTS_PER_CAMERA-1).
 * Returns false if the index is out of range.
 */
bool getWireTarget(uint32_t pointIndex, WireTarget& out);

/**
 * Describe the next point the user should click for a camera.
 * Returns false when the camera is already fully calibrated.
 */
bool getNextWireTarget(uint32_t camIndex, WireTarget& out);

/**
 * Warp a source camera frame into the canonical 720x720 dartboard view using
 * the camera's stored homography. Returns false if the camera is not yet
 * calibrated.
 */
bool warpCameraFrame(uint32_t camIndex, const cv::Mat& src, cv::Mat& dst);

/**
 * The same warp for a binary mask, sampled nearest-neighbour.
 *
 * Separate from warpCameraFrame because the interpolation is part of the
 * contract, not a preference: DartModelTraining's precompute_real_masks.py
 * thresholds at native resolution and then warps with INTER_NEAREST, so the
 * conditioning masks the model was trained on are strictly 0 or 255. Bilinear
 * would put a grey fringe on every dart edge and hand the model values it has
 * never seen.
 *
 * Returns false if the camera is not yet calibrated.
 */
bool warpCameraMask(uint32_t camIndex, const cv::Mat& src, cv::Mat& dst);

/**
 * Copy a camera's homography into `out` as 9 row-major doubles.
 *
 * Maps source-image pixels to the 720x720 canonical view — the same convention
 * as DartModelTraining's board_detections.json, so a value from here can be fed
 * straight to cv2.warpPerspective(img, H, (720, 720)) or merged into that file.
 *
 * Returns false when the camera is not calibrated.
 */
bool getCameraHomography(uint32_t camIndex, double out[9]);

/**
 * Save all calibrated points to a text file (./config/wire_calibration.txt
 * by default). Returns STATUS_OK on success.
 */
Status saveWireCalibration(const std::string& path = appDataPath("config/wire_calibration.txt"));

/**
 * Load wire calibration from a text file. Cameras with fewer than
 * WIRE_POINTS_PER_CAMERA points in the file are skipped. Recomputes the
 * homography for any fully-calibrated cameras.
 */
Status loadWireCalibration(const std::string& path = appDataPath("config/wire_calibration.txt"));

#endif // WIRE_CALIBRATION_HPP
