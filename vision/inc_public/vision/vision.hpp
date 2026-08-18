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
#include "detect/wire_calibration.hpp"  // EXPECTED_CAMERA_COUNT
#include <cstdint>
#include <functional>
#include <string>
#include <vector>


// ============================================================================
// Vision module lifecycle
// ============================================================================

/**
 * Initialize the vision module. Creates the appropriate vision source
 * based on build configuration (sim when TREBLOTRON_USE_SIM is defined).
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
 * Returns true once if the user explicitly asked for the board to be
 * reset (e.g. clicked the sim's Collect Darts button) since the last
 * call. Distinct from isBoardClear() — fires for an already-clear board
 * too, so a turn can be force-ended even when no darts ever landed.
 */
bool consumeBoardResetRequest();

/**
 * Copy the latest dart-detection heatmap out of the active vision source.
 * Returns false when the source doesn't produce a heatmap (e.g. the sim) or
 * no inference has run yet. On success `out` holds width*height floats in
 * row-major order, values roughly in [0, 1].
 */
bool getLatestVisionHeatmap(std::vector<float>& out,
                            uint32_t& width, uint32_t& height);


// ============================================================================
// Async-init state (TensorRT engine build, etc.)
// ============================================================================

/**
 * True if the active vision source is still initializing (e.g. the
 * TensorRT source is building its FP16 engine). The main loop should
 * render a loading screen via presentVisionLoadingFrame() while this
 * returns true.
 *
 * For sources that come up synchronously (sim) this always
 * returns false after initializeVisionModule() returns.
 */
bool isVisionInitializing();

/**
 * True if the vision source's init failed permanently (e.g. the
 * TensorRT engine couldn't be built, or the ONNX is missing required
 * tensor names). The main loop should keep the loading screen up while
 * this is true so the user can see the error before quitting.
 */
bool isVisionFailed();

/** Monotonic init progress in [0, 1]. 1.0 when not initializing. */
float getVisionInitProgress();

/**
 * "Still doing something" iteration counter — TRT step counter
 * accumulated across every internal phase. Useful as an activity
 * indicator on the loading screen during long phases where the bar
 * doesn't visibly move. 0 when no source is active.
 */
uint64_t getVisionInitIteration();

/**
 * Short human-readable phase label for the loading screen
 * (e.g. "Building dart detector"). On failure this holds the error
 * message ("Failed: ...") and the loading screen renders it
 * prominently. Empty when no source is active.
 */
std::string getVisionInitStatus();

/**
 * Short human-readable runtime detection status — e.g. "Detecting",
 * "Detecting (entering 1/2)", "Removing (clear 5/10)". Empty for
 * sources without a state machine (sim) and when no source is active.
 * Intended for the vision_debug overlay.
 */
std::string getVisionDetectionStatus();

/**
 * Whether this build actually detects darts from camera frames.
 *
 * False only for the simulated source, which invents darts and has no
 * thresholds to tune. The settings screen asks so it can leave out controls
 * that would do nothing — a build-time fact, exposed as a function so the
 * ifdefs stay inside the vision module.
 */
bool visionHasDetector();

/**
 * Whether inference happens on a remote server rather than on this machine.
 *
 * True only for the network source. What makes the server address worth showing
 * — and what makes a settings change something that has to travel over a wire.
 */
bool visionUsesRemoteServer();

/**
 * Render one frame of the "building model" loading screen to the main
 * window (the frame created by the game manager). Intended to be called
 * from a mini-loop between initializeVisionModule() and the normal game
 * loop when isVisionInitializing() is true.
 *
 * Clears the frame, renders the loading UI, flushes the render queue,
 * and presents the frame — callers should not wrap it in
 * renderQueueDrawFlush / presentFrame themselves.
 *
 * deltaTime drives the spinner animation. Safe to call even when vision
 * is not actively initializing.
 */
void presentVisionLoadingFrame(float deltaTime);


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
 * Monotonic counter of frames published for this camera, 0 if unknown.
 * A consumer that is polled faster than the cameras run uses this to tell a
 * genuinely new frame from one it has already handled.
 */
uint64_t getCameraFrameSequence(uint32_t index);

/**
 * Copies the latest warped (720x720 template-space) RGB frame for the given
 * camera into outFrame. Returns false if the camera is not calibrated, has no
 * frame yet, or the index is out of range. The warp is performed on the
 * camera capture thread so readers never pay warp cost on the hot path.
 */
bool getCameraWarpedFrame(uint32_t index, CameraFrame& outFrame);

#ifdef TREBLOTRON_PASSTHROUGH_CAPTURE
/**
 * Copies the latest frame for a camera as JPEG bytes, ready to put on a wire.
 *
 * The UVC cameras already deliver MJPEG, so in the normal case this hands back
 * the sensor's own compressed buffer with no decode, no re-encode, and no
 * second generation of compression loss. If the driver refused to give up raw
 * MJPEG, it falls back to encoding from the decoded frame — same result, more
 * CPU. Only available in builds that stream frames to a remote server.
 */
bool getCameraCompressedFrame(uint32_t index, std::vector<uint8_t>& out);
#endif

#ifdef TREBLOTRON_HAVE_LOCAL_INFERENCE
/**
 * Copies the latest dart-segmentation input plane for the given camera into the
 * caller-provided buffer (NCHW float32, shape (3, 360, 640) — i.e. 3*360*640
 * floats). Produced on the capture thread by resizing the raw RGB to 640x360
 * and dividing by 255, in parallel with the other cameras' threads, so the
 * inference thread can just memcpy the planes into the seg engine's pinned
 * input buffer with no per-pixel work. Returns false if the camera has no
 * frame yet, the index is out of range, or `floatCount` is too small.
 *
 * Only available in local-inference builds — the remote and sim paths
 * have no use for it.
 */
bool getCameraSegPlane(uint32_t index, float* out, size_t floatCount);

/**
 * Publish a warped frame produced *outside* the camera capture thread (the TRT
 * pipeline warps the seg-masked frame on the inference thread, then hands it
 * back here so vision_debug can keep showing a per-camera preview without
 * paying for a second warp). The pixel buffer is copied into the slot under
 * the slot lock; subsequent getCameraWarpedFrame() calls return it. Pixel
 * format is RGB24 packed (3 bytes/pixel, row-major, `stride` bytes per row).
 */
void publishCameraWarpedFrame(uint32_t index,
                              const uint8_t* pixels,
                              int width, int height, int stride);
#endif

/**
 * Swap which physical camera occupies the two given logical slots. Used on
 * boot enumerations where the OS hands us cameras in a different order than
 * the saved wire calibrations expect: the calibration data is keyed by logical
 * slot and stays put, so swapping moves the live feeds to line up with their
 * calibrations. Returns false on out-of-range index or no-op (a == b).
 */
bool swapCameraSlots(uint32_t a, uint32_t b);

/**
 * Save a capture of the frames currently being scored.
 *
 * On a remote-inference build this asks the server, which holds the exact
 * frames the model saw and can write their canonical warps alongside for free.
 * Everywhere else it writes the local camera frames. Returns STATUS_OK when the
 * request was made; for the remote path the outcome arrives later via
 * consumeVisionCaptureResult().
 */
Status saveVisionCapture(const std::string& outputDir = appDataPath("captures"));

/**
 * Result text of the last remote capture, once, or empty. The UI polls this so
 * it can replace "requested" with the path the server actually wrote.
 */
std::string consumeVisionCaptureResult();

/**
 * Save the current frame from every connected camera to the output directory.
 * All images in one capture share the same UUID: {uuid}_cam0.png, {uuid}_cam1.png, ...
 * Creates the output directory if it does not exist.
 */
Status saveAllCameraFrames(const std::string& outputDir);

#endif // VISION_HPP
