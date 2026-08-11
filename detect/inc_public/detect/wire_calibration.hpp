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
 * Get the human-readable label for the next point to place
 * (e.g. "Triple 5/20" or "Double 12/20"). Empty if camera is fully calibrated.
 */
std::string getNextPointLabel(uint32_t camIndex);

/**
 * Warp a source camera frame into the canonical 720x720 dartboard view using
 * the camera's stored homography. Returns false if the camera is not yet
 * calibrated.
 */
bool warpCameraFrame(uint32_t camIndex, const cv::Mat& src, cv::Mat& dst);

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
Status saveWireCalibration(const std::string& path = "./config/wire_calibration.txt");

/**
 * Load wire calibration from a text file. Cameras with fewer than
 * WIRE_POINTS_PER_CAMERA points in the file are skipped. Recomputes the
 * homography for any fully-calibrated cameras.
 */
Status loadWireCalibration(const std::string& path = "./config/wire_calibration.txt");

#endif // WIRE_CALIBRATION_HPP
