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
#include "game_lib/game.hpp"
#include <cstdint>
#include <string>

struct SDL_Surface;


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


// ============================================================================
// Game connection
// ============================================================================

/**
 * Connect or disconnect a game from the vision source.
 * The connected game receives dart event callbacks (onDartLanded,
 * onDartPositionCalculated). Pass nullptr to disconnect.
 */
void setVisionGame(GamePtr game);


// ============================================================================
// Camera API (for calibration / data collection)
// ============================================================================

/** The system expects exactly 3 cameras. */
static constexpr uint32_t EXPECTED_CAMERA_COUNT = 3;

/** Initialize the camera system. Call before any other camera functions. */
Status initializeCameraSystem();

/** Shut down the camera system and release resources. */
void shutdownCameraSystem();

/** Returns the number of currently connected cameras. */
uint32_t getCameraCount();

/** Returns a display name for the camera at the given index. */
std::string getCameraName(uint32_t index);

/**
 * Returns the latest frame from the camera at the given index.
 * The returned surface is owned by the camera system — do NOT free it.
 * Returns nullptr if no frame is available.
 */
SDL_Surface* getCameraFrame(uint32_t index);

/**
 * Save the current frame from the given camera to the output directory.
 * The filename includes a UUID for uniqueness.
 * Creates the output directory if it does not exist.
 */
Status saveCameraFrame(uint32_t cameraIndex, const std::string& outputDir);

#endif // VISION_HPP
