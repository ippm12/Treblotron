/**
 * vision.hpp
 *
 * Public API for the vision module. Provides dart detection via
 * camera or simulation, and camera access for data collection.
 * All internal classes (VisionSource, SimVisionSource) are hidden
 * behind this function-based API.
 */

#ifndef VISION_HPP
#define VISION_HPP

#include "common_inc.hpp"
#include <cstdint>
#include <functional>
#include <string>
#include <vector>


// ============================================================================
// Vision module lifecycle
// ============================================================================

/**
 * Initialize the vision module. Creates the appropriate vision source
 * based on build configuration (sim when DARTLENS_USE_SIM is defined).
 * Must be called after initializeFrameModule() and initializeGameManager().
 */
Status initializeVisionModule();

/** Shut down the vision module and release all resources. */
void shutdownVisionModule();


// ============================================================================
// Per-frame update
// ============================================================================

/** Called once per frame by the game manager with delta time in seconds. */
void tickVision(float deltaTime);


// ============================================================================
// Board state
// ============================================================================

/**
 * Returns true if the vision source reports no darts on the board.
 * Returns true if no vision source is active.
 */
bool isBoardClear();

/** Clear all tracked darts and reset the vision source's board state. */
void resetVisionDarts();

/**
 * Copy the latest dart-detection heatmap out of the active vision source.
 * Returns false when the source doesn't produce a heatmap (e.g. the sim) or
 * no inference has run yet. On success `out` holds width*height floats in
 * row-major order, values roughly in [0, 1].
 */
bool getLatestVisionHeatmap(std::vector<float>& out,
                            uint32_t& width, uint32_t& height);


// ============================================================================
// Game connection (via callbacks — vision module knows nothing about Game)
// ============================================================================

using DartLandedCallback   = std::function<void()>;
using DartPositionCallback = std::function<void(float angle, float normalizedRadius)>;

/**
 * Register callbacks that fire when darts are detected.
 * Pass nullptr to disconnect. The game manager typically registers these
 * and forwards events to the current game.
 */
void setVisionCallbacks(DartLandedCallback onDartLanded,
                        DartPositionCallback onDartPositionCalculated);


// ============================================================================
// Camera API (for calibration / data collection)
// ============================================================================

/** The system expects exactly 3 cameras. */
static constexpr uint32_t EXPECTED_CAMERA_COUNT = 3;

/** Frame data copied out of the camera system. Caller owns the pixel data. */
struct CameraFrame
{
    std::vector<uint8_t> pixels;  // RGB24 pixel data
    int width  = 0;
    int height = 0;
    int stride = 0;               // bytes per row
};

/** Initialize the camera system. Call before any other camera functions. */
Status initializeCameraSystem();

/** Shut down the camera system and release resources. */
void shutdownCameraSystem();

/** Returns the number of currently connected cameras. */
uint32_t getCameraCount();

/** Returns a display name for the camera at the given index. */
std::string getCameraName(uint32_t index);

/**
 * Copies the latest frame from the camera at the given index into outFrame.
 * Returns true if a new frame was available, false otherwise.
 * The caller owns outFrame and may use it for as long as needed.
 */
bool getCameraFrame(uint32_t index, CameraFrame& outFrame);

/**
 * Save the current frame from every connected camera to the output directory.
 * All images in one capture share the same UUID: {uuid}_cam0.png, {uuid}_cam1.png, ...
 * Creates the output directory if it does not exist.
 */
Status saveAllCameraFrames(const std::string& outputDir);

#endif // VISION_HPP
