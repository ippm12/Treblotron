/**
 * network_vision_source.cpp
 *
 * Client half of the remote-inference link. One background thread owns the
 * socket for its whole lifetime: connect, handshake, then a credit-driven
 * send/receive loop. The main thread only ever touches the mutex-guarded event
 * queue and the atomics, exactly like the local source.
 *
 * The server address comes from vision_link (./config/server.txt, seeded from
 * DARTLENS_SERVER on first run) and is re-read on every reconnect, so editing it
 * in the settings screen takes effect within a retry interval without a restart.
 */

#include "network_vision_source.hpp"
#include "detect/wire_calibration.hpp"
#include "net/dart_protocol.hpp"
#include "net/net_socket.hpp"
#include "vision/vision_link.hpp"

#include <array>
#include <cstdio>
#include <chrono>
#include <cstdlib>
#include <thread>


namespace
{
    constexpr uint16_t DEFAULT_PORT = 9876;

    /** Wait between connection attempts. The server may not be up yet. */
    constexpr int RECONNECT_DELAY_MS = 2000;

    /**
     * Read timeout on the socket. Long enough to cover a slow CPU inference
     * cycle plus a first-run engine build's progress gaps, short enough that a
     * server that dies mid-session doesn't wedge the client forever.
     */
    constexpr int READ_TIMEOUT_MS = 30000;

    /** See the server side — sized to hold a whole in-flight frame set. */
    constexpr int SOCKET_BUFFER_BYTES = 4 * 1024 * 1024;

    /**
     * Read-poll slice. Also the worst case between a camera producing a frame
     * and us forwarding it while holding a credit, so keep it well under the
     * ~33 ms camera period.
     */
    constexpr int CLIENT_POLL_MS = 2;

    /**
     * Round-trip above this reads as amber. Healthy is ~30 ms end to end, so
     * 150 ms means something is genuinely struggling — a saturated link, or a
     * server sharing its GPU — while still leaving room for ordinary jitter.
     */
    constexpr float DEGRADED_RTT_MS = 150.0f;

    /**
     * Connected but nothing scored for this long also reads as amber: the
     * socket being up is not the same thing as darts being counted.
     */
    constexpr int64_t STALL_MS = 2000;

    /** Weight of each new sample. Slow enough that one bad cycle can't flip it. */
    constexpr float RTT_SMOOTHING = 0.25f;


    /**
     * Split the configured address into host and port.
     *
     * Read fresh on every reconnect rather than cached at startup, which is
     * what lets an edit in the settings screen take effect without a restart.
     * Returns false when nothing is configured — the caller reports that
     * plainly instead of quietly trying localhost.
     */
    bool resolveServerAddress(std::string& host, uint16_t& port)
    {
        const std::string configured = getInferenceServerAddress();
        if(configured.empty()) return false;

        port = DEFAULT_PORT;

        const size_t colon = configured.rfind(':');
        if(colon == std::string::npos)
        {
            host = configured;
            return true;
        }

        host = configured.substr(0, colon);
        const int parsed = std::atoi(configured.c_str() + colon + 1);
        if(parsed > 0 && parsed <= 65535) port = static_cast<uint16_t>(parsed);
        return !host.empty();
    }
}


NetworkVisionSource::NetworkVisionSource() = default;


NetworkVisionSource::~NetworkVisionSource() { shutdown(); }


Status NetworkVisionSource::init()
{
    // The cameras and the calibration are ours — only inference is remote.
    initializeCameraSystem();
    initializeWireCalibration();
    if(IS_STATUS_NOT_OK(loadWireCalibration()))
    {
        LOG_WARNING(VISION_LOG_ID,
                    "NetworkVisionSource: no wire calibration loaded — run the "
                    "calibration screen before the server will accept us");
    }

    m_running.store(true, std::memory_order_release);
    m_thread = std::thread(&NetworkVisionSource::clientThreadMain, this);
    return STATUS_OK;
}


void NetworkVisionSource::clientThreadMain()
{
    while(m_running.load(std::memory_order_acquire))
    {
        if(!runSession() && m_running.load(std::memory_order_acquire))
        {
            m_connected.store(false, std::memory_order_release);
            m_ready.store(false, std::memory_order_release);
            m_smoothedRttMs.store(-1.0f, std::memory_order_release);
            m_lastDetectionMs.store(0, std::memory_order_release);
            std::this_thread::sleep_for(std::chrono::milliseconds(RECONNECT_DELAY_MS));
        }
    }
}


bool NetworkVisionSource::runSession()
{
    auto setInitStatus = [&](const std::string& s)
    {
        std::lock_guard<std::mutex> lock(m_statusMutex);
        m_initStatus = s;
    };

    std::string host;
    uint16_t    port = DEFAULT_PORT;
    if(!resolveServerAddress(host, port))
    {
        setInitStatus("No inference server configured");
        std::this_thread::sleep_for(std::chrono::milliseconds(RECONNECT_DELAY_MS));
        return false;
    }

    setInitStatus("Connecting to " + host + ":" + std::to_string(port));

    NetSocket sock;
    if(IS_STATUS_NOT_OK(sock.connect(host, port)))
    {
        setInitStatus("Waiting for inference server at " + host + ":" + std::to_string(port));
        return false;
    }
    sock.setNoDelay(true);
    sock.setReadTimeout(READ_TIMEOUT_MS);
    // Matches the server: enough send buffer to hand a whole frame set to the
    // kernel and get straight back to waiting, rather than blocking in send()
    // while the server is mid-inference.
    sock.setBufferSizes(SOCKET_BUFFER_BYTES, SOCKET_BUFFER_BYTES);

    LOG_INFO(VISION_LOG_ID, "NetworkVisionSource: connected to {}:{}", host, port);
    m_connected.store(true, std::memory_order_release);
    setInitStatus("Connected — waiting for server models");

    // ---- Handshake -------------------------------------------------------
    // Send the raw wire points rather than a homography: the server runs them
    // through the same fit, so both ends warp identically by construction.
    DartHello hello;
    hello.cameraCount  = getCameraCount();
    hello.nativeWidth  = 1280;
    hello.nativeHeight = 720;
    hello.wantHeatmap  = true;   // cheap enough, and vision_debug needs it
    hello.wirePointsXY.resize(EXPECTED_CAMERA_COUNT);
    for(uint32_t cam = 0; cam < EXPECTED_CAMERA_COUNT; cam++)
    {
        const uint32_t count = getWirePointCount(cam);
        hello.wirePointsXY[cam].reserve(count * 2);
        for(uint32_t i = 0; i < count; i++)
        {
            float x = 0.0f, y = 0.0f;
            if(getWirePoint(cam, i, x, y))
            {
                hello.wirePointsXY[cam].push_back(x);
                hello.wirePointsXY[cam].push_back(y);
            }
        }
    }

    if(!dartSendHello(sock, hello)) return false;

    DartMsgHeader        header;
    std::vector<uint8_t> payload;
    if(!dartRecvHeader(sock, header) || !dartRecvPayload(sock, header, payload)) return false;
    if(static_cast<DartMsg>(header.type) != DartMsg::HelloAck) return false;

    DartHelloAck ack;
    if(!dartParseHelloAck(payload, ack)) return false;

    if(ack.status == DartHandshake::Busy)
    {
        // Transient: the server is serving another board. Keep retrying, and
        // say so rather than sitting on a bare "connecting" message — this is
        // the one failure a user can resolve themselves.
        LOG_INFO(VISION_LOG_ID, "NetworkVisionSource: server busy — {}", ack.message);
        setInitStatus("Inference server is busy (" + ack.message + ")");
        return false;
    }

    if(ack.status != DartHandshake::Accepted)
    {
        // A rejected handshake is a configuration problem — a retry loop would
        // just hammer the server, so this is one of the few genuinely terminal
        // states the loading screen reports.
        LOG_ERROR(VISION_LOG_ID, "NetworkVisionSource: server rejected us: {}", ack.message);
        setInitStatus("Failed: " + ack.message);
        m_failed.store(true, std::memory_order_release);
        m_running.store(false, std::memory_order_release);
        return false;
    }

    // ---- Serve loop ------------------------------------------------------
    uint32_t sequence = 0;
    std::array<uint64_t, EXPECTED_CAMERA_COUNT> lastSeq{};

    // Holding a credit must never stop us reading. The server credits faster
    // than the cameras produce, so we routinely wait for a fresh capture — and
    // blocking in that wait would leave dart detections sitting unread in the
    // socket, adding a camera period to every scoring event. Poll instead, and
    // send the moment we have both a credit and something new to send.
    bool creditHeld = false;
    auto lastHeard  = std::chrono::steady_clock::now();

    // Sends the newest captures, but only once at least one camera has produced
    // a frame we have not already forwarded — a duplicate costs full bandwidth
    // and tells the detector nothing it doesn't already know.
    auto sendFramesIfFresh = [&]() -> bool
    {
        const uint32_t camCount = getCameraCount();

        bool fresh = false;
        for(uint32_t cam = 0; cam < camCount && cam < EXPECTED_CAMERA_COUNT; cam++)
        {
            if(getCameraFrameSequence(cam) != lastSeq[cam]) { fresh = true; break; }
        }
        if(!fresh) return false;

        DartFrames frames;
        frames.sequence    = ++sequence;
        frames.captureTime = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
        frames.jpegs.resize(EXPECTED_CAMERA_COUNT);

        // Straight from camera_api as JPEG. The cameras already deliver MJPEG,
        // so in the normal case these are the sensor's own bytes — no decode,
        // no re-encode, no second generation of compression loss.
        for(uint32_t cam = 0; cam < EXPECTED_CAMERA_COUNT; cam++)
        {
            if(cam >= camCount) continue;
            lastSeq[cam] = getCameraFrameSequence(cam);
            if(!getCameraCompressedFrame(cam, frames.jpegs[cam]))
            {
                frames.jpegs[cam].clear();
            }
        }

        m_sent[frames.sequence % RTT_RING] = {frames.sequence, std::chrono::steady_clock::now()};
        return dartSendFrames(sock, frames);
    };

    while(m_running.load(std::memory_order_acquire))
    {
        if(m_resetRequested.exchange(false, std::memory_order_acq_rel))
        {
            if(!dartSendSimple(sock, DartMsg::Reset)) return false;
        }

        if(!sock.waitReadable(CLIENT_POLL_MS))
        {
            const auto idleMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - lastHeard).count();
            if(idleMs >= READ_TIMEOUT_MS)
            {
                LOG_WARNING(VISION_LOG_ID,
                            "NetworkVisionSource: no word from the server for {} s", idleMs / 1000);
                return false;
            }
            if(creditHeld && sendFramesIfFresh()) creditHeld = false;
            continue;
        }
        lastHeard = std::chrono::steady_clock::now();

        if(!dartRecvHeader(sock, header) || !dartRecvPayload(sock, header, payload))
        {
            LOG_WARNING(VISION_LOG_ID, "NetworkVisionSource: link to server lost");
            return false;
        }

        switch(static_cast<DartMsg>(header.type))
        {
            case DartMsg::InitProgress:
            {
                DartInitProgress msg;
                if(!dartParseInitProgress(payload, msg)) return false;

                m_initProgress.store(msg.progress, std::memory_order_release);
                m_initIteration.store(msg.iteration, std::memory_order_relaxed);
                setInitStatus(msg.status);

                if(msg.state == DartInitState::Ready)
                {
                    m_ready.store(true, std::memory_order_release);
                }
                else if(msg.state == DartInitState::Failed)
                {
                    LOG_ERROR(VISION_LOG_ID, "NetworkVisionSource: server build failed: {}",
                              msg.status);
                    m_failed.store(true, std::memory_order_release);
                    m_running.store(false, std::memory_order_release);
                    return false;
                }
                break;
            }

            case DartMsg::WantFrames:
            {
                uint32_t credits = 0;
                if(!dartParseWantFrames(payload, credits)) return false;
                if(credits > 0) creditHeld = true;
                break;
            }

            case DartMsg::Detection:
            {
                DartDetectionMsg msg;
                if(!dartParseDetection(payload, msg)) return false;

                const auto now = std::chrono::steady_clock::now();
                m_lastDetectionMs.store(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        now.time_since_epoch()).count(), std::memory_order_release);

                // Only sequences still in the ring can be timed; with
                // continuous streaming most sends never get a detection of
                // their own, and an unmatched slot just means it aged out.
                const SentStamp& stamp = m_sent[msg.sequence % RTT_RING];
                if(stamp.sequence == msg.sequence)
                {
                    const float rtt = static_cast<float>(
                        std::chrono::duration_cast<std::chrono::microseconds>(
                            now - stamp.at).count()) / 1000.0f;
                    const float prev = m_smoothedRttMs.load(std::memory_order_relaxed);
                    m_smoothedRttMs.store(prev < 0.0f ? rtt
                                          : prev + RTT_SMOOTHING * (rtt - prev),
                                          std::memory_order_release);
                }

                m_boardClear.store(msg.boardClear, std::memory_order_release);
                {
                    std::lock_guard<std::mutex> lock(m_statusMutex);
                    m_detectionStatus = msg.status;
                }
                if(!msg.darts.empty())
                {
                    std::lock_guard<std::mutex> lock(m_eventMutex);
                    for(const DartDetectionMsg::Dart& d : msg.darts)
                    {
                        m_newDartEvents.emplace(d.angle, d.normalizedRadius);
                    }
                }
                break;
            }

            case DartMsg::Heatmap:
            {
                DartHeatmapMsg msg;
                if(!dartParseHeatmap(payload, msg)) return false;

                std::vector<float> heat(msg.values.size());
                for(size_t i = 0; i < msg.values.size(); i++)
                {
                    heat[i] = static_cast<float>(msg.values[i]) / 255.0f;
                }
                std::lock_guard<std::mutex> lock(m_heatmapMutex);
                m_latestHeatmap  = std::move(heat);
                m_latestHeatmapW = msg.width;
                m_latestHeatmapH = msg.height;
                break;
            }

            default:
                LOG_WARNING(VISION_LOG_ID,
                            "NetworkVisionSource: ignoring unexpected message {}",
                            header.type);
                break;
        }

        if(creditHeld && sendFramesIfFresh()) creditHeld = false;
    }

    dartSendSimple(sock, DartMsg::Bye);
    return true;
}


void NetworkVisionSource::shutdown()
{
    if(!m_running.exchange(false, std::memory_order_acq_rel) && !m_thread.joinable())
    {
        return;
    }
    m_running.store(false, std::memory_order_release);
    if(m_thread.joinable()) m_thread.join();

    shutdownWireCalibration();
    shutdownCameraSystem();
    LOG_INFO(VISION_LOG_ID, "NetworkVisionSource shut down");
}


void NetworkVisionSource::tick(float deltaTime)
{
    (void)deltaTime;

    std::queue<std::pair<float, float>> drained;
    {
        std::lock_guard<std::mutex> lock(m_eventMutex);
        std::swap(drained, m_newDartEvents);
    }

    while(!drained.empty())
    {
        const auto& [angle, normR] = drained.front();
        if(m_onDartLanded)             m_onDartLanded();
        if(m_onDartPositionCalculated) m_onDartPositionCalculated(angle, normR);
        drained.pop();
    }
}


bool NetworkVisionSource::isBoardClear() const
{
    return m_boardClear.load(std::memory_order_acquire);
}


void NetworkVisionSource::resetDarts()
{
    m_resetRequested.store(true, std::memory_order_release);
}


bool NetworkVisionSource::getLatestHeatmap(std::vector<float>& out,
                                           uint32_t& width, uint32_t& height) const
{
    std::lock_guard<std::mutex> lock(m_heatmapMutex);
    if(m_latestHeatmap.empty()) return false;
    out    = m_latestHeatmap;
    width  = m_latestHeatmapW;
    height = m_latestHeatmapH;
    return true;
}


bool NetworkVisionSource::isInitializing() const
{
    // Connecting counts as initializing: the server may simply not be up yet,
    // and the loading screen is the right place to say so.
    return !m_failed.load(std::memory_order_acquire)
        && !m_ready.load(std::memory_order_acquire);
}


bool NetworkVisionSource::isFailed() const
{
    return m_failed.load(std::memory_order_acquire);
}


float NetworkVisionSource::getInitProgress() const
{
    if(m_ready.load(std::memory_order_acquire)) return 1.0f;
    if(!m_connected.load(std::memory_order_acquire)) return 0.0f;
    return m_initProgress.load(std::memory_order_acquire);
}


uint64_t NetworkVisionSource::getInitIteration() const
{
    return m_initIteration.load(std::memory_order_relaxed);
}


std::string NetworkVisionSource::getInitStatus() const
{
    std::lock_guard<std::mutex> lock(m_statusMutex);
    return m_initStatus;
}


std::string NetworkVisionSource::getDetectionStatus() const
{
    std::lock_guard<std::mutex> lock(m_statusMutex);
    return m_detectionStatus;
}


VisionLinkState NetworkVisionSource::getLinkState() const
{
    if(!m_connected.load(std::memory_order_acquire)) return VisionLinkState::Disconnected;

    // Connected is not the same as working. A link that is up but scoring
    // nothing — server still loading, or wedged — is amber, not green.
    const int64_t last = m_lastDetectionMs.load(std::memory_order_acquire);
    if(last == 0) return VisionLinkState::Degraded;

    const int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    if(nowMs - last > STALL_MS) return VisionLinkState::Degraded;

    const float rtt = m_smoothedRttMs.load(std::memory_order_acquire);
    if(rtt > DEGRADED_RTT_MS) return VisionLinkState::Degraded;

    return VisionLinkState::Healthy;
}


std::string NetworkVisionSource::getLinkDetail() const
{
    const std::string configured = getInferenceServerAddress();
    if(configured.empty()) return "no server configured";

    if(!m_connected.load(std::memory_order_acquire))
    {
        std::lock_guard<std::mutex> lock(m_statusMutex);
        return m_initStatus;
    }

    const float rtt = m_smoothedRttMs.load(std::memory_order_acquire);
    if(rtt < 0.0f) return configured + " - connected, no frames scored yet";

    char buf[64];
    std::snprintf(buf, sizeof(buf), " - %.0f ms", static_cast<double>(rtt));
    return configured + buf;
}
