/**
 * dart_protocol.hpp
 *
 * Wire protocol between the game client (Raspberry Pi: cameras + UI) and the
 * inference server (a machine that can actually afford to run the models).
 *
 * Shape of a session:
 *
 *   client                                server
 *     |------------- Hello --------------->|   proto version, wire points
 *     |<----------- HelloAck --------------|
 *     |<--------- InitProgress ------------|   repeated while models load
 *     |<--------- WantFrames(n) -----------|   flow-control credit
 *     |------------- Frames -------------->|   3x JPEG
 *     |<---------- Detection --------------|   dart events + board state
 *     |<---------- Heatmap ----------------|   optional, debug overlay only
 *     |------------- Reset --------------->|   collect-darts / new turn
 *
 * Credit-based flow control is load-bearing, not a nicety. Inference is far
 * slower than the camera frame rate, so a free-running stream would queue up
 * seconds of stale frames and the server would be scoring the past. The server
 * grants one triple at a time, which pins the pipeline to "always analysing
 * the newest frames the client has".
 *
 * All integers are little-endian; all floats are IEEE-754 binary32. Both ends
 * of this link are little-endian (ARM and x86-64 alike), so encoding is a
 * straight memcpy — but everything goes through these helpers so that
 * assumption lives in exactly one place.
 */

#ifndef NET_DART_PROTOCOL_HPP
#define NET_DART_PROTOCOL_HPP

#include "common_inc.hpp"
#include "net/net_socket.hpp"

#include <cstdint>
#include <string>
#include <vector>


/** Bump on any incompatible change; HelloAck rejects a mismatch. */
constexpr uint32_t DART_PROTOCOL_VERSION = 1;

/** 'DLNS' — sanity check that we're framed correctly on the stream. */
constexpr uint32_t DART_PROTOCOL_MAGIC = 0x534E4C44u;

/** Refuse absurd payloads rather than trying to allocate them. */
constexpr uint32_t DART_PROTOCOL_MAX_PAYLOAD = 32u * 1024u * 1024u;


enum class DartMsg : uint16_t
{
    // client → server
    Hello        = 1,
    Frames       = 2,
    Reset        = 3,
    Bye          = 4,
    SaveCapture  = 5,

    // server → client
    HelloAck     = 128,
    InitProgress = 129,
    WantFrames   = 130,
    Detection    = 131,
    Heatmap      = 132,
    CaptureSaved = 133,
};


/** Fixed 12-byte message header. */
struct DartMsgHeader
{
    uint32_t magic        = DART_PROTOCOL_MAGIC;
    uint16_t type         = 0;
    uint16_t flags        = 0;
    uint32_t payloadBytes = 0;
};

// The header goes onto the wire as raw bytes, so any padding a compiler
// inserted would silently desync the two ends. The field order above is
// already padding-free under natural alignment; this makes that load-bearing
// rather than incidental.
static_assert(sizeof(DartMsgHeader) == 12, "DartMsgHeader must be exactly 12 bytes on the wire");


/** Server-side model-loading state, mirrored to the client's loading screen. */
enum class DartInitState : uint8_t
{
    Connecting = 0,  // client-side only: not connected yet
    Building   = 1,
    Ready      = 2,
    Failed     = 3,
};


// ============================================================================
// Message payloads
// ============================================================================

struct DartHello
{
    uint32_t protocolVersion = DART_PROTOCOL_VERSION;
    uint32_t cameraCount     = 0;
    uint32_t nativeWidth     = 0;
    uint32_t nativeHeight    = 0;
    bool     wantHeatmap     = false;

    /**
     * Raw clicked wire points per camera, in source-image pixels. The client
     * sends points rather than a homography so the server can run them through
     * the same addWirePoint()/findHomography path — one implementation of the
     * fit and the remap-table bake, so both ends warp identically.
     */
    std::vector<std::vector<float>> wirePointsXY;  // [cam][2*n], x,y interleaved
};


/**
 * Outcome of a handshake. The distinction between Rejected and Busy matters to
 * the client: one means "stop trying, a human has to fix something", the other
 * means "come back shortly". Collapsing them would either strand a client that
 * only needed to wait, or have it hammer a server it can never join.
 */
enum class DartHandshake : uint32_t
{
    Rejected = 0,  // terminal — protocol mismatch, no usable calibration
    Accepted = 1,
    Busy     = 2,  // transient — the server is serving another client
};


struct DartHelloAck
{
    uint32_t      protocolVersion = DART_PROTOCOL_VERSION;
    DartHandshake status          = DartHandshake::Rejected;
    uint32_t      maxInFlight     = 1;
    std::string   message;
};


struct DartInitProgress
{
    float         progress  = 0.0f;  // 0..1
    DartInitState state     = DartInitState::Building;
    uint64_t      iteration = 0;
    std::string   status;
};


struct DartFrames
{
    uint32_t sequence    = 0;
    uint64_t captureTime = 0;  // microseconds, client clock; for latency logging

    /** Per camera, a JPEG. Empty means that camera had no frame this cycle. */
    std::vector<std::vector<uint8_t>> jpegs;
};


struct DartDetectionMsg
{
    uint32_t sequence   = 0;   // echoes the DartFrames it was computed from
    bool     boardClear = true;

    struct Dart { float angle; float normalizedRadius; };
    std::vector<Dart> darts;

    std::string status;
};


struct DartHeatmapMsg
{
    uint32_t width  = 0;
    uint32_t height = 0;
    /** Quantized sigmoid values: 0..255 maps linearly onto [0, 1]. */
    std::vector<uint8_t> values;
};


// ============================================================================
// Send / receive
// ============================================================================

/**
 * Read one message header. Returns false on EOF, socket error, bad magic, or a
 * payload larger than DART_PROTOCOL_MAX_PAYLOAD.
 */
bool dartRecvHeader(NetSocket& sock, DartMsgHeader& out);

/** Read `header.payloadBytes` into `out`. */
bool dartRecvPayload(NetSocket& sock, const DartMsgHeader& header,
                     std::vector<uint8_t>& out);

/** Every sender below frames the payload with a header in a single writeAll. */
bool dartSendHello       (NetSocket& sock, const DartHello& msg);
bool dartSendHelloAck    (NetSocket& sock, const DartHelloAck& msg);
bool dartSendInitProgress(NetSocket& sock, const DartInitProgress& msg);
bool dartSendWantFrames  (NetSocket& sock, uint32_t credits);
bool dartSendFrames      (NetSocket& sock, const DartFrames& msg);
bool dartSendDetection   (NetSocket& sock, const DartDetectionMsg& msg);
bool dartSendHeatmap     (NetSocket& sock, const DartHeatmapMsg& msg);
bool dartSendSimple      (NetSocket& sock, DartMsg type);   // Reset, Bye

/**
 * Result of a SaveCapture. The frames a capture is meant to preserve are the
 * ones the server actually scored, so the save happens there and this reports
 * back what landed on disk.
 */
struct DartCaptureSaved
{
    bool        ok = false;
    std::string message;   // path written, or why it failed
};

bool dartSendCaptureSaved(NetSocket& sock, const DartCaptureSaved& msg);

/** Decoders. Each returns false when the payload is malformed or truncated. */
bool dartParseHello       (const std::vector<uint8_t>& payload, DartHello& out);
bool dartParseHelloAck    (const std::vector<uint8_t>& payload, DartHelloAck& out);
bool dartParseInitProgress(const std::vector<uint8_t>& payload, DartInitProgress& out);
bool dartParseWantFrames  (const std::vector<uint8_t>& payload, uint32_t& credits);
bool dartParseFrames      (const std::vector<uint8_t>& payload, DartFrames& out);
bool dartParseDetection   (const std::vector<uint8_t>& payload, DartDetectionMsg& out);
bool dartParseHeatmap     (const std::vector<uint8_t>& payload, DartHeatmapMsg& out);
bool dartParseCaptureSaved(const std::vector<uint8_t>& payload, DartCaptureSaved& out);

#endif // NET_DART_PROTOCOL_HPP
