/**
 * server.hpp
 *
 * Headless dart-inference server. Accepts camera frames from a game client
 * over TCP, runs the shared DartDetector on them, and returns dart events.
 *
 * This exists so the game can run on hardware too small for the models (a
 * Raspberry Pi) while inference happens on a machine that can afford it. The
 * detection code itself is not duplicated — both ends link the same
 * detect module, so a dart scored here is scored exactly as it would be
 * locally.
 *
 * The server builds its models on a background thread and starts listening
 * immediately, so a client that connects during a first-run TensorRT compile
 * sees real progress on its loading screen instead of a refused connection.
 */

#ifndef SERVER_HPP
#define SERVER_HPP

#include "common_inc.hpp"
#include "detect/dart_detector.hpp"

#include <atomic>
#include <cstdint>
#include <string>


struct DartServerConfig
{
    uint16_t    port     = 9876;
    std::string modelDir = "./detect/models";

    /** Forwarded to DartDetectorConfig. Build-time: needs a restart to change. */
    bool enableHandFilter = true;

    /**
     * Starting detection settings, from the command line.
     *
     * These are the defaults the server runs on, not the last word. A connected
     * client sends its own — the settings screen is on the board, where the
     * person adjusting them can see the effect — and that overrides these for
     * the life of the session. What is left here is what --replay uses, and
     * what a session runs on before its client has said anything.
     */
    DartVisionSettings settings;

    /** Where SaveCapture writes: native frames plus a {stem}.json of transforms. */
    std::string captureDir = "./captures";

    /**
     * Honour a client's request for heatmap frames. Costs ~127 KB per cycle,
     * so it exists for the vision_debug overlay and can be forced off here.
     */
    bool allowHeatmap = true;

    /**
     * How long a read may stall before the session is abandoned. A client that
     * loses power or drops off Wi-Fi never sends a TCP FIN, so without this the
     * server would block on the dead socket forever and no one could reconnect.
     * Generous: the only reads are replies the client sends immediately.
     */
    int readTimeoutMs = 20000;

    /**
     * After a session drops, how long the board state (confirmed darts, the
     * Detecting/Removing state) is held for the same machine to come back. A
     * brief network blip mid-turn shouldn't cost the player the darts already
     * thrown. Reconnects after this, or from a different machine, start clean.
     */
    int reconnectGraceMs = 60000;
};


/**
 * Start the models loading (on a background thread) and bind the listen
 * socket. Returns an error only if the socket can't be bound — a model-load
 * failure surfaces later, through InitProgress, so connected clients can
 * display it.
 */
Status initializeServerModule(const DartServerConfig& config);

/**
 * Serve clients until `stop` becomes true. One client at a time: a second
 * connection is accepted only after the first disconnects. Blocking.
 */
void runServer(const std::atomic<bool>& stop);

void shutdownServerModule();


/**
 * Load both dart models, run one forward pass on zeroed input, and log the
 * output shapes and timings. Verifies the backend and the model contract
 * without needing a camera, a client, or a calibration. Returns STATUS_OK if
 * everything loaded and ran.
 */
Status runServerSelfTest(const DartServerConfig& config);

/**
 * Feed saved camera triples through the detector instead of the network.
 * `captureDir` holds the PNG sets written by saveAllCameraFrames():
 * {uuid}_cam0.png, {uuid}_cam1.png, {uuid}_cam2.png. Requires a wire
 * calibration at `calibrationPath`. Logs a decoded dart position per triple.
 */
Status runServerReplay(const DartServerConfig& config,
                       const std::string& captureDir,
                       const std::string& calibrationPath);

#endif // SERVER_HPP
