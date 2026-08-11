/**
 * server.cpp
 *
 * Accept loop, client session, and the offline selftest/replay modes.
 *
 * Threading: the model build runs on f_buildThread so the listener can come up
 * immediately. A live session then runs on two threads — a reader that drains
 * the socket into a one-slot newest-wins mailbox, and the accept thread, which
 * does the inference. Splitting them is what lets the client keep streaming
 * fresh frames while a cycle is still running; a single thread parked in
 * inference cannot drain, so the client would be stuck holding a spent credit
 * and the next set scored would be as old as the slowest cycle.
 *
 * Only one client is served at a time, so the detector itself needs no locking.
 * SessionState carries what the two threads share, including a mutex around
 * sends because both of them write to the socket.
 */

#include "server/server.hpp"
#include "detect/wire_calibration.hpp"
#include "net/dart_protocol.hpp"
#include "net/net_socket.hpp"

#include <opencv2/core.hpp>
#include <opencv2/core/utility.hpp>   // cv::parallel_for_
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <map>
#include <mutex>
#include <thread>


namespace
{
    DartServerConfig f_config;
    DartDetector     f_detector;
    NetListener      f_listener;

    std::thread            f_buildThread;
    std::atomic<bool>      f_buildAbort{false};
    std::atomic<DartInitState> f_buildState{DartInitState::Building};
    std::atomic<float>     f_buildProgress{0.0f};
    std::atomic<uint64_t>  f_buildIteration{0};
    std::mutex             f_buildStatusMutex;
    std::string            f_buildStatus = "Starting";

    /** How often to re-send InitProgress to a client waiting on the build. */
    constexpr int BUILD_POLL_MS = 250;

    /** Accept wakes up this often so runServer can notice `stop`. */
    constexpr int ACCEPT_POLL_MS = 250;

    /**
     * How finely a live session slices its wait on the client. Also the worst
     * case a second client waits to be told the server is busy. Short enough to
     * feel immediate, long enough that an idle session costs nothing.
     */
    constexpr int BUSY_POLL_MS = 100;

    /**
     * Kernel socket buffer size. Comfortably larger than a three-camera JPEG
     * set (~270 KB at 720p) so a full set can land while inference is running.
     * Windows defaults to 64 KB, which would break the overlap outright.
     */
    constexpr int SOCKET_BUFFER_BYTES = 4 * 1024 * 1024;


    void setBuildStatus(const std::string& s)
    {
        std::lock_guard<std::mutex> lock(f_buildStatusMutex);
        f_buildStatus = s;
    }

    std::string buildStatus()
    {
        std::lock_guard<std::mutex> lock(f_buildStatusMutex);
        return f_buildStatus;
    }


    DartDetectorConfig detectorConfig()
    {
        DartDetectorConfig dc;
        dc.modelDir           = f_config.modelDir;
        dc.enableHandFilter   = f_config.enableHandFilter;
        dc.confirmFrames      = f_config.confirmFrames;
        dc.clearHoldMs        = f_config.clearHoldMs;
        dc.handEnterMs        = f_config.handEnterMs;
        return dc;
    }


    void buildThreadMain()
    {
        auto onProgress = [](float pct, uint64_t iter, const std::string& phase)
        {
            // Monotonic — never let a phase regress the client's bar.
            const float prev = f_buildProgress.load(std::memory_order_relaxed);
            if(pct > prev) f_buildProgress.store(pct, std::memory_order_release);
            if(iter) f_buildIteration.store(iter, std::memory_order_relaxed);
            if(!phase.empty()) setBuildStatus(phase);
        };

        const Status stat = f_detector.build(detectorConfig(), onProgress, f_buildAbort);
        if(IS_STATUS_NOT_OK(stat))
        {
            setBuildStatus("Failed: could not load models");
            f_buildState.store(DartInitState::Failed, std::memory_order_release);
            LOG_ERROR(SERVER_LOG_ID, "model build failed — clients will be told to give up");
            return;
        }

        f_buildProgress.store(1.0f, std::memory_order_release);
        setBuildStatus("Ready");
        f_buildState.store(DartInitState::Ready, std::memory_order_release);
        LOG_INFO(SERVER_LOG_ID, "models ready on {} — accepting frames",
                 f_detector.backendName());
    }


    bool sendInitProgress(NetSocket& sock)
    {
        DartInitProgress msg;
        msg.progress  = f_buildProgress.load(std::memory_order_acquire);
        msg.state     = f_buildState.load(std::memory_order_acquire);
        msg.iteration = f_buildIteration.load(std::memory_order_relaxed);
        msg.status    = buildStatus();
        return dartSendInitProgress(sock, msg);
    }


    /**
     * Install the client's wire points into the local calibration state. Runs
     * them through addWirePoint() so the homography fit and the remap-table
     * bake are the same code the client would have used.
     */
    bool installCalibration(const DartHello& hello)
    {
        initializeWireCalibration();

        uint32_t calibrated = 0;
        for(uint32_t cam = 0; cam < EXPECTED_CAMERA_COUNT; cam++)
        {
            clearWirePoints(cam);
            if(cam >= hello.wirePointsXY.size()) continue;

            const std::vector<float>& pts = hello.wirePointsXY[cam];
            if(pts.size() % 2 != 0)
            {
                LOG_ERROR(SERVER_LOG_ID, "camera {} sent an odd number of point floats ({})",
                          cam, pts.size());
                return false;
            }
            for(size_t i = 0; i + 1 < pts.size(); i += 2)
            {
                addWirePoint(cam, pts[i], pts[i + 1]);
            }
            if(isCameraCalibrated(cam)) calibrated++;
        }

        LOG_INFO(SERVER_LOG_ID, "installed client calibration: {}/{} cameras calibrated",
                 calibrated, EXPECTED_CAMERA_COUNT);
        return calibrated > 0;
    }


    /**
     * Fabricate a plausible 40-point wire calibration for every camera so the
     * selftest can run the warp stages without a real rig. Mirrors the ring
     * geometry wire_calibration.cpp uses for its template points, laid out
     * around the centre of a 1280x720 frame — the resulting homography is a
     * near-similarity, which is all the pipeline needs to execute.
     */
    void installSyntheticCalibration()
    {
        constexpr double PI = 3.14159265358979323846;
        constexpr float  WIRE_START_ANGLE_DEG = 279.0f;
        constexpr float  SEGMENT_ANGLE_DEG    = 18.0f;
        constexpr float  RING_RATIOS[2]       = {0.629f, 1.000f};
        constexpr uint32_t POINTS_PER_RING    = WIRE_POINTS_PER_CAMERA / 2;
        constexpr float  CX = 640.0f, CY = 360.0f, R = 300.0f;

        for(uint32_t cam = 0; cam < EXPECTED_CAMERA_COUNT; cam++)
        {
            clearWirePoints(cam);
            for(uint32_t ring = 0; ring < 2; ring++)
            {
                for(uint32_t i = 0; i < POINTS_PER_RING; i++)
                {
                    const float deg = WIRE_START_ANGLE_DEG
                                    + static_cast<float>(i) * SEGMENT_ANGLE_DEG;
                    const float rad = deg * static_cast<float>(PI) / 180.0f;
                    const float rr  = R * RING_RATIOS[ring];
                    addWirePoint(cam, CX + rr * std::cos(rad), CY + rr * std::sin(rad));
                }
            }
        }
    }


    /**
     * Decode the client's JPEGs into native-resolution RGB Mats.
     *
     * The three cameras decode concurrently. Done serially this was roughly
     * 15 ms of a ~36 ms cycle — the largest single cost left after the GPU made
     * inference cheap. The decodes are completely independent, so they map
     * straight onto OpenCV's existing thread pool with no extra machinery and
     * no per-cycle thread creation.
     *
     * IMREAD_COLOR_RGB decodes directly to RGB. The obvious IMREAD_COLOR gives
     * BGR and would need a full-frame cvtColor per camera afterwards, which is
     * pure waste when libjpeg can emit the channel order we want in the first
     * place.
     */
    bool decodeFrames(const DartFrames& msg,
                      std::array<cv::Mat, EXPECTED_CAMERA_COUNT>& out)
    {
        std::array<bool, EXPECTED_CAMERA_COUNT> decoded{};

        cv::parallel_for_(cv::Range(0, static_cast<int>(EXPECTED_CAMERA_COUNT)),
            [&](const cv::Range& range)
            {
                for(int i = range.start; i < range.end; i++)
                {
                    const size_t cam = static_cast<size_t>(i);
                    out[cam] = cv::Mat();
                    if(cam >= msg.jpegs.size() || msg.jpegs[cam].empty()) continue;

                    out[cam] = cv::imdecode(msg.jpegs[cam], cv::IMREAD_COLOR_RGB);
                    if(out[cam].empty())
                    {
                        LOG_WARNING(SERVER_LOG_ID,
                                    "camera {} sent an undecodable JPEG ({} bytes)",
                                    cam, msg.jpegs[cam].size());
                        continue;
                    }
                    decoded[cam] = true;
                }
            });

        for(bool d : decoded)
        {
            if(d) return true;
        }
        return false;
    }


    /**
     * Write the frame set just scored, plus the canonical warps derived from
     * it, under `captureDir`.
     *
     * Saved here rather than on the client because these are the exact frames
     * the model saw. Newest-wins drop means the client is already holding a
     * later set by the time a button reaches us, so a client-side save would
     * preserve the aftermath of a miss rather than the miss.
     *
     * Layout mirrors the training repo: {stem}_camN.png natively, and the
     * matching 720x720 warp under transformed/, so a capture can be dropped
     * straight into data/images + data/transformed, or replayed with --replay.
     */
    std::string saveCapture(const std::array<cv::Mat, EXPECTED_CAMERA_COUNT>& frames,
                            const std::string& captureDir, bool& ok)
    {
        ok = false;
        std::error_code ec;
        std::filesystem::create_directories(captureDir, ec);
        std::filesystem::create_directories(captureDir + "/transformed", ec);
        if(ec) return "could not create " + captureDir;

        // Timestamped stem: sortable, and lines up with the server log so a
        // capture can be matched to the cycle that produced it.
        const auto now = std::chrono::system_clock::now();
        const auto t   = std::chrono::system_clock::to_time_t(now);
        const auto ms  = std::chrono::duration_cast<std::chrono::milliseconds>(
                             now.time_since_epoch()).count() % 1000;
        char stem[32];
        std::tm tm{};
#ifdef _WIN32
        localtime_s(&tm, &t);
#else
        localtime_r(&t, &tm);
#endif
        std::snprintf(stem, sizeof(stem), "%04d%02d%02d-%02d%02d%02d-%03d",
                      tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                      tm.tm_hour, tm.tm_min, tm.tm_sec, static_cast<int>(ms));

        uint32_t written = 0;
        cv::Mat bgr;
        for(uint32_t cam = 0; cam < EXPECTED_CAMERA_COUNT; cam++)
        {
            if(frames[cam].empty()) continue;

            // Everything in the pipeline is RGB; imwrite expects BGR.
            cv::cvtColor(frames[cam], bgr, cv::COLOR_RGB2BGR);
            const std::string raw = captureDir + "/" + stem + "_cam"
                                  + std::to_string(cam) + ".png";
            if(!cv::imwrite(raw, bgr))
            {
                LOG_ERROR(SERVER_LOG_ID, "could not write {}", raw);
                continue;
            }
            written++;

            const cv::Mat& warped = f_detector.warpedFrame(cam);
            if(warped.empty()) continue;
            cv::cvtColor(warped, bgr, cv::COLOR_RGB2BGR);
            cv::imwrite(captureDir + "/transformed/" + stem + "_cam"
                        + std::to_string(cam) + ".png", bgr);
        }

        if(written == 0) return "no usable frames to save";

        ok = true;
        const std::string where = captureDir + "/" + stem + "_cam*.png";
        LOG_INFO(SERVER_LOG_ID, "saved capture: {} ({} cameras, warps included)",
                 where, written);
        return where;
    }


    bool sendHeatmapIfWanted(NetSocket& sock, bool wanted, std::mutex& sendMutex)
    {
        if(!wanted || !f_config.allowHeatmap) return true;

        std::vector<float> heat;
        uint32_t w = 0, h = 0;
        if(!f_detector.latestHeatmap(heat, w, h)) return true;

        DartHeatmapMsg msg;
        msg.width  = w;
        msg.height = h;
        msg.values.resize(heat.size());
        for(size_t i = 0; i < heat.size(); i++)
        {
            // Quantize [0,1] to a byte — 1/255 resolution is far finer than
            // the overlay can show, and it keeps this at ~127 KB instead of
            // ~500 KB per cycle.
            const float v = std::clamp(heat[i], 0.0f, 1.0f);
            msg.values[i] = static_cast<uint8_t>(v * 255.0f + 0.5f);
        }
        std::lock_guard<std::mutex> lock(sendMutex);
        return dartSendHeatmap(sock, msg);
    }


    /**
     * Shared between a session's reader thread and its inference loop.
     *
     * `latest` is a one-slot mailbox, not a queue: a set that arrives while an
     * older one is still waiting simply overwrites it. Dropping the stale set
     * is the whole point — the newest frames are the only ones worth scoring,
     * and keeping a backlog would just push detection further into the past.
     */
    struct SessionState
    {
        std::mutex              mutex;
        std::condition_variable frameReady;
        DartFrames              latest;
        bool                    hasFrame = false;
        uint64_t                dropped  = 0;   // overwritten before being scored

        /** Set by either thread when the link is finished with. */
        std::atomic<bool> linkDown{false};

        /** Applied by the inference loop, so a reset can't land mid-run(). */
        std::atomic<bool> resetPending{false};

        /**
         * Serviced by the inference loop right after a cycle, so the frames
         * written are the ones just scored — not whatever happens to be in
         * flight when the request arrives.
         */
        std::atomic<bool> savePending{false};

        /** Both threads write to the socket, so every send takes this. */
        std::mutex sendMutex;
    };


    /**
     * Drains the socket for the lifetime of a session.
     *
     * Re-credits the client the instant a frame set comes off the wire, rather
     * than when inference finishes with it. That is what keeps the client
     * streaming fresh data continuously instead of sitting on a spent credit
     * while a long cycle runs.
     */
    void readerMain(NetSocket& sock, SessionState& st, const std::atomic<bool>& stop,
                    const std::string& peer, int readTimeoutMs)
    {
        auto sendCredit = [&]() -> bool
        {
            std::lock_guard<std::mutex> lock(st.sendMutex);
            return dartSendWantFrames(sock, 1);
        };

        if(!sendCredit())
        {
            st.linkDown = true;
            st.frameReady.notify_all();
            return;
        }

        DartMsgHeader        header;
        std::vector<uint8_t> payload;
        auto lastHeard = std::chrono::steady_clock::now();

        while(!stop.load(std::memory_order_acquire) && !st.linkDown.load(std::memory_order_acquire))
        {
            if(!sock.waitReadable(BUSY_POLL_MS))
            {
                const auto idleMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - lastHeard).count();
                if(idleMs >= readTimeoutMs)
                {
                    // A client that lost power or dropped off Wi-Fi never sends
                    // a FIN, so silence is the only signal we get.
                    LOG_WARNING(SERVER_LOG_ID,
                                "client {} went silent for {} s — dropping the session",
                                peer, idleMs / 1000);
                    break;
                }
                continue;
            }

            if(!dartRecvHeader(sock, header) || !dartRecvPayload(sock, header, payload))
            {
                break;   // client went away
            }
            lastHeard = std::chrono::steady_clock::now();

            switch(static_cast<DartMsg>(header.type))
            {
                case DartMsg::Frames:
                {
                    DartFrames msg;
                    if(!dartParseFrames(payload, msg))
                    {
                        LOG_ERROR(SERVER_LOG_ID, "malformed Frames from {}", peer);
                        st.linkDown = true;
                        st.frameReady.notify_all();
                        return;
                    }

                    {
                        std::lock_guard<std::mutex> lock(st.mutex);
                        if(st.hasFrame) st.dropped++;   // newest wins
                        st.latest   = std::move(msg);
                        st.hasFrame = true;
                    }
                    st.frameReady.notify_one();

                    // Credit immediately: keep the client sending.
                    if(!sendCredit()) { st.linkDown = true; st.frameReady.notify_all(); return; }
                    break;
                }

                case DartMsg::Reset:
                    LOG_INFO(SERVER_LOG_ID, "client {} requested a board reset", peer);
                    st.resetPending.store(true, std::memory_order_release);
                    break;

                case DartMsg::SaveCapture:
                    LOG_INFO(SERVER_LOG_ID, "client {} requested a capture", peer);
                    st.savePending.store(true, std::memory_order_release);
                    break;

                case DartMsg::Bye:
                    LOG_INFO(SERVER_LOG_ID, "client {} said goodbye", peer);
                    st.linkDown = true;
                    st.frameReady.notify_all();
                    return;

                default:
                    LOG_WARNING(SERVER_LOG_ID, "ignoring unexpected message {} from {}",
                                header.type, peer);
                    break;
            }
        }

        st.linkDown = true;
        st.frameReady.notify_all();
    }


    /**
     * Tell a client arriving mid-session to come back later, and close.
     *
     * Its Hello is read first rather than replying blind: closing a socket with
     * unread data queued makes the stack send an RST, which can discard the
     * reply we just wrote. Draining it first means the client reliably sees the
     * Busy answer instead of a connection reset it would have to guess about.
     */
    void rejectBusy(NetSocket sock, const std::string& activePeer)
    {
        const std::string peer = sock.peerName();

        sock.setReadTimeout(1000);
        DartMsgHeader        header;
        std::vector<uint8_t> payload;
        if(dartRecvHeader(sock, header))
        {
            dartRecvPayload(sock, header, payload);
        }

        DartHelloAck ack;
        ack.status  = DartHandshake::Busy;
        ack.message = "server is already serving " + activePeer;
        dartSendHelloAck(sock, ack);

        LOG_INFO(SERVER_LOG_ID, "turned away {} — busy with {}", peer, activePeer);
    }


    /**
     * Serve one connected client until it disconnects or errors out.
     *
     * `resume` keeps the detector's board state instead of clearing it, for a
     * client reconnecting inside the grace window.
     */
    void runSession(NetSocket sock, const std::atomic<bool>& stop, bool resume)
    {
        const std::string peer = sock.peerName();
        LOG_INFO(SERVER_LOG_ID, "client connected from {}{}", peer,
                 resume ? " (resuming, board state kept)" : "");

        sock.setNoDelay(true);
        sock.setReadTimeout(f_config.readTimeoutMs);
        // Room for the whole in-flight frame set. The client pushes set N+1
        // while we are inferring on set N and not reading the socket, so if it
        // does not fit here the client stalls mid-send and we lose the overlap
        // the early credit grant exists to create.
        sock.setBufferSizes(SOCKET_BUFFER_BYTES, SOCKET_BUFFER_BYTES);

        DartMsgHeader        header;
        std::vector<uint8_t> payload;

        // ---- Handshake --------------------------------------------------
        if(!dartRecvHeader(sock, header) || !dartRecvPayload(sock, header, payload))
        {
            LOG_WARNING(SERVER_LOG_ID, "client {} disconnected before Hello", peer);
            return;
        }
        if(static_cast<DartMsg>(header.type) != DartMsg::Hello)
        {
            LOG_ERROR(SERVER_LOG_ID, "client {} sent message {} before Hello",
                      peer, header.type);
            return;
        }

        DartHello hello;
        if(!dartParseHello(payload, hello))
        {
            LOG_ERROR(SERVER_LOG_ID, "client {} sent a malformed Hello", peer);
            return;
        }

        DartHelloAck ack;
        if(hello.protocolVersion != DART_PROTOCOL_VERSION)
        {
            ack.status = DartHandshake::Rejected;
            ack.message  = "protocol version mismatch: server speaks "
                         + std::to_string(DART_PROTOCOL_VERSION)
                         + ", client speaks " + std::to_string(hello.protocolVersion);
            LOG_ERROR(SERVER_LOG_ID, "{}", ack.message);
            dartSendHelloAck(sock, ack);
            return;
        }
        if(!installCalibration(hello))
        {
            ack.status = DartHandshake::Rejected;
            ack.message  = "no camera was fully calibrated — run the calibration screen "
                           "on the client first";
            LOG_ERROR(SERVER_LOG_ID, "{}", ack.message);
            dartSendHelloAck(sock, ack);
            return;
        }

        ack.status      = DartHandshake::Accepted;
        ack.maxInFlight = 1;
        ack.message     = "ok";
        if(!dartSendHelloAck(sock, ack)) return;

        LOG_INFO(SERVER_LOG_ID, "client {} accepted: {} cameras at {}x{}, heatmap={}",
                 peer, hello.cameraCount, hello.nativeWidth, hello.nativeHeight,
                 hello.wantHeatmap ? "on" : "off");

        // ---- Wait for the models, keeping the client's loading screen fed --
        while(!stop.load(std::memory_order_acquire))
        {
            const DartInitState state = f_buildState.load(std::memory_order_acquire);
            if(!sendInitProgress(sock)) return;
            if(state == DartInitState::Ready)  break;
            if(state == DartInitState::Failed) return;  // client shows the error and stops
            std::this_thread::sleep_for(std::chrono::milliseconds(BUILD_POLL_MS));
        }

        // Fresh client, fresh board — otherwise a new client would inherit the
        // darts confirmed during someone else's session. A reconnect inside
        // the grace window deliberately keeps them: a Wi-Fi blip mid-turn
        // shouldn't cost the player darts they already threw.
        if(!resume) f_detector.reset();

        // ---- Frame loop ---------------------------------------------------
        //
        // A reader thread owns the socket's read side and drains it
        // continuously, keeping only the newest complete frame set. Inference
        // runs here and takes whatever is newest when it becomes free.
        //
        // Simply granting the next credit early is not enough. That bounds the
        // pipeline but not the staleness: with one set queued, a cycle that
        // runs long leaves the client holding no credit and unable to refresh
        // the set already waiting, so what we score next was captured when the
        // slow cycle *started*. Staleness ends up bounded by the worst cycle
        // time, which is backwards.
        //
        // Re-crediting the moment a set comes off the wire fixes that: the
        // client streams continuously at link rate, the slot keeps being
        // overwritten with fresher data, and inference always picks up the
        // newest thing that has arrived. Staleness is bounded by one transfer,
        // regardless of how long any individual cycle takes. Credits still cap
        // the wire to one set in flight, so nothing queues up in the network.
        SessionState st;
        std::thread reader(readerMain, std::ref(sock), std::ref(st),
                           std::ref(stop), std::cref(peer), f_config.readTimeoutMs);

        std::array<cv::Mat, EXPECTED_CAMERA_COUNT> frames;
        DartDetectorResult result;
        uint64_t cycles = 0;
        bool     lastBoardClear = true;   // matches the freshly-reset detector

        while(!stop.load(std::memory_order_acquire))
        {
            // Turn away anyone trying to connect while we are busy.
            NetSocket extra = f_listener.accept(0);
            if(extra.isOpen()) rejectBusy(std::move(extra), peer);

            DartFrames msg;
            uint64_t   skipped = 0;
            {
                std::unique_lock<std::mutex> lock(st.mutex);
                st.frameReady.wait_for(lock, std::chrono::milliseconds(BUSY_POLL_MS),
                                       [&]{ return st.hasFrame || st.linkDown; });
                if(st.linkDown) break;
                if(!st.hasFrame) continue;   // nothing yet — go poll the listener again

                msg          = std::move(st.latest);
                st.hasFrame  = false;
                skipped      = st.dropped;
                st.dropped   = 0;
            }

            if(st.resetPending.exchange(false, std::memory_order_acq_rel))
            {
                // Applied here rather than on the reader thread so it can never
                // land in the middle of a run().
                f_detector.reset();
                lastBoardClear = true;
            }

            const auto t0 = std::chrono::steady_clock::now();

            DartDetectionMsg reply;
            reply.sequence = msg.sequence;

            const bool haveFrames = decodeFrames(msg, frames);
            const auto tDecoded   = std::chrono::steady_clock::now();

            if(haveFrames && f_detector.run(frames, nullptr, result))
            {
                reply.boardClear = result.boardClear;
                reply.status     = result.status;
                for(const DartDetection& d : result.newDarts)
                {
                    reply.darts.push_back({d.angle, d.normalizedRadius});
                    LOG_INFO(SERVER_LOG_ID, "dart: angle={:.1f} r={:.3f}",
                             d.angle, d.normalizedRadius);
                }
            }
            else
            {
                // Nothing usable this cycle (no calibrated camera with a frame,
                // or an inference hiccup). We must not invent a board state:
                // the game ends a turn on the not-clear → clear transition, so
                // reporting "clear" here would cut a turn short over a single
                // dropped frame. Repeat the last real answer.
                reply.boardClear = lastBoardClear;
                reply.status     = "no usable frames";
            }
            lastBoardClear = reply.boardClear;

            {
                std::lock_guard<std::mutex> sendLock(st.sendMutex);
                if(!dartSendDetection(sock, reply)) break;
            }
            if(!sendHeatmapIfWanted(sock, hello.wantHeatmap, st.sendMutex)) break;

            if(st.savePending.exchange(false, std::memory_order_acq_rel))
            {
                DartCaptureSaved saved;
                saved.message = saveCapture(frames, f_config.captureDir, saved.ok);
                std::lock_guard<std::mutex> sendLock(st.sendMutex);
                if(!dartSendCaptureSaved(sock, saved)) break;
            }

            // Reported per stage: "is the pipeline fast enough" is a question
            // about where the time actually goes, and a single total invites
            // guessing.
            const auto now      = std::chrono::steady_clock::now();
            const auto decodeUs = std::chrono::duration_cast<std::chrono::microseconds>(
                tDecoded - t0).count();
            const auto totalUs  = std::chrono::duration_cast<std::chrono::microseconds>(
                now - t0).count();
            if(++cycles % 30 == 0)
            {
                LOG_INFO(SERVER_LOG_ID,
                         "cycle {}: {:.1f} ms total = {:.1f} decode + {:.1f} detect+reply "
                         "({:.1f} Hz), {} newer set(s) skipped — {}",
                         cycles, totalUs / 1000.0, decodeUs / 1000.0,
                         (totalUs - decodeUs) / 1000.0,
                         totalUs > 0 ? 1e6 / static_cast<double>(totalUs) : 0.0,
                         skipped, reply.status);
            }
        }

        st.linkDown = true;
        st.frameReady.notify_all();
        sock.close();          // unblocks the reader if it is parked in recv
        if(reader.joinable()) reader.join();

        LOG_INFO(SERVER_LOG_ID, "client {} disconnected after {} cycles", peer, cycles);
    }
}


// ============================================================================
// Lifecycle
// ============================================================================

Status initializeServerModule(const DartServerConfig& config)
{
    f_config = config;

    if(IS_STATUS_NOT_OK(f_listener.listen(config.port)))
    {
        LOG_CRITICAL(SERVER_LOG_ID, "could not listen on port {}", config.port);
        return STATUS_ERROR_GENERIC;
    }

    // Models load in the background so a client connecting during a first-run
    // engine build gets a progress bar instead of a refused connection.
    f_buildState.store(DartInitState::Building, std::memory_order_release);
    setBuildStatus("Loading models");
    f_buildThread = std::thread(buildThreadMain);

    LOG_INFO(SERVER_LOG_ID, "server listening on port {}, models from {}",
             config.port, config.modelDir);
    return STATUS_OK;
}


void runServer(const std::atomic<bool>& stop)
{
    // Board state is held briefly after a session ends so the same machine can
    // reconnect and carry on mid-turn. Tracked by address, not by socket: the
    // port changes on every reconnect, the machine does not.
    std::string lastPeer;
    std::chrono::steady_clock::time_point endedAt{};
    bool graceHeld = false;

    auto graceRemainingMs = [&]() -> long long
    {
        return f_config.reconnectGraceMs
             - std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now() - endedAt).count();
    };

    while(!stop.load(std::memory_order_acquire))
    {
        NetSocket client = f_listener.accept(ACCEPT_POLL_MS);

        if(!client.isOpen())
        {
            // Poll timeout. Expire a stale grace window here rather than on the
            // next connect, so the board doesn't silently keep old darts for an
            // arbitrarily long time waiting for someone to show up.
            if(graceHeld && graceRemainingMs() <= 0)
            {
                LOG_INFO(SERVER_LOG_ID,
                         "{} did not return within {} s — clearing the board",
                         lastPeer, f_config.reconnectGraceMs / 1000);
                f_detector.reset();
                graceHeld = false;
            }
            continue;
        }

        const std::string peer = client.peerAddress();
        const bool resume = graceHeld && !peer.empty() && peer == lastPeer
                         && graceRemainingMs() > 0;

        runSession(std::move(client), stop, resume);

        lastPeer  = peer;
        endedAt   = std::chrono::steady_clock::now();
        graceHeld = true;
    }
}


void shutdownServerModule()
{
    f_buildAbort.store(true, std::memory_order_release);
    if(f_buildThread.joinable()) f_buildThread.join();

    f_listener.close();
    f_detector.shutdown();
    shutdownWireCalibration();
    LOG_INFO(SERVER_LOG_ID, "server shut down");
}


// ============================================================================
// Offline modes
// ============================================================================

Status runServerSelfTest(const DartServerConfig& config)
{
    f_config = config;

    LOG_INFO(SERVER_LOG_ID, "selftest: loading models from {}", config.modelDir);

    std::atomic<bool> noAbort{false};
    auto onProgress = [](float pct, uint64_t, const std::string& phase)
    {
        LOG_INFO(SERVER_LOG_ID, "  [{:3.0f}%] {}", pct * 100.0f, phase);
    };

    if(IS_STATUS_NOT_OK(f_detector.build(detectorConfig(), onProgress, noAbort)))
    {
        LOG_CRITICAL(SERVER_LOG_ID, "selftest FAILED: models did not load");
        return STATUS_ERROR_GENERIC;
    }

    // Run the whole pipeline, not just the loads. A synthetic calibration is
    // enough: the frames are noise, so the geometry is meaningless, but every
    // stage still executes — seg forward, mask, warp, 10-channel pack, AR
    // forward, decode, state machine. That is what actually catches a changed
    // model contract, which loading alone would not.
    LOG_INFO(SERVER_LOG_ID, "selftest: running on {}", f_detector.backendName());
    LOG_INFO(SERVER_LOG_ID, "selftest: models loaded, exercising the full pipeline");

    initializeWireCalibration();
    installSyntheticCalibration();

    std::array<cv::Mat, EXPECTED_CAMERA_COUNT> frames;
    for(auto& f : frames)
    {
        f.create(720, 1280, CV_8UC3);
        cv::randu(f, cv::Scalar::all(0), cv::Scalar::all(255));
    }

    DartDetectorResult result;
    constexpr int SELFTEST_CYCLES = 3;
    bool ran = false;
    int64_t lastMs = 0;

    for(int i = 0; i < SELFTEST_CYCLES; i++)
    {
        const auto t0 = std::chrono::steady_clock::now();
        ran = f_detector.run(frames, nullptr, result);
        lastMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
        if(!ran) break;
        LOG_INFO(SERVER_LOG_ID, "selftest: cycle {} took {} ms — {}", i + 1, lastMs, result.status);
    }

    if(!ran)
    {
        LOG_CRITICAL(SERVER_LOG_ID, "selftest FAILED: the pipeline did not complete a cycle");
        f_detector.shutdown();
        shutdownWireCalibration();
        return STATUS_ERROR_GENERIC;
    }

    LOG_INFO(SERVER_LOG_ID,
             "selftest PASSED — {} ms/cycle steady state (~{:.1f} Hz)",
             lastMs, lastMs > 0 ? 1000.0 / static_cast<double>(lastMs) : 0.0);
    f_detector.shutdown();
    shutdownWireCalibration();
    return STATUS_OK;
}


Status runServerReplay(const DartServerConfig& config,
                       const std::string& captureDir,
                       const std::string& calibrationPath)
{
    namespace fs = std::filesystem;
    f_config = config;

    initializeWireCalibration();
    if(IS_STATUS_NOT_OK(loadWireCalibration(calibrationPath)))
    {
        LOG_CRITICAL(SERVER_LOG_ID, "replay: could not load calibration from {}",
                     calibrationPath);
        return STATUS_ERROR_GENERIC;
    }

    uint32_t calibrated = 0;
    for(uint32_t cam = 0; cam < EXPECTED_CAMERA_COUNT; cam++)
    {
        if(isCameraCalibrated(cam)) calibrated++;
    }
    if(calibrated == 0)
    {
        LOG_CRITICAL(SERVER_LOG_ID, "replay: {} has no fully-calibrated camera",
                     calibrationPath);
        return STATUS_ERROR_GENERIC;
    }

    // Group the capture directory into {uuid} -> per-camera paths. The layout
    // is what saveAllCameraFrames() writes: {uuid}_cam0.png, _cam1, _cam2.
    std::map<std::string, std::array<std::string, EXPECTED_CAMERA_COUNT>> sets;
    std::error_code ec;
    for(const auto& entry : fs::directory_iterator(captureDir, ec))
    {
        if(ec) break;
        if(!entry.is_regular_file()) continue;

        const std::string stem = entry.path().stem().string();
        const size_t marker = stem.rfind("_cam");
        if(marker == std::string::npos || marker + 4 >= stem.size()) continue;

        const char camChar = stem[marker + 4];
        if(camChar < '0' || camChar >= static_cast<char>('0' + EXPECTED_CAMERA_COUNT)) continue;

        sets[stem.substr(0, marker)][static_cast<size_t>(camChar - '0')] =
            entry.path().string();
    }

    if(sets.empty())
    {
        LOG_CRITICAL(SERVER_LOG_ID, "replay: no {{uuid}}_camN.* files found in {}", captureDir);
        return STATUS_ERROR_GENERIC;
    }
    LOG_INFO(SERVER_LOG_ID, "replay: {} capture sets in {}", sets.size(), captureDir);

    std::atomic<bool> noAbort{false};
    auto onProgress = [](float pct, uint64_t, const std::string& phase)
    {
        LOG_INFO(SERVER_LOG_ID, "  [{:3.0f}%] {}", pct * 100.0f, phase);
    };
    if(IS_STATUS_NOT_OK(f_detector.build(detectorConfig(), onProgress, noAbort)))
    {
        LOG_CRITICAL(SERVER_LOG_ID, "replay: models did not load");
        return STATUS_ERROR_GENERIC;
    }

    LOG_INFO(SERVER_LOG_ID, "replay: running on {}", f_detector.backendName());

    std::array<cv::Mat, EXPECTED_CAMERA_COUNT> frames;
    DartDetectorResult result;

    for(const auto& [uuid, paths] : sets)
    {
        uint32_t loaded = 0;
        for(uint32_t cam = 0; cam < EXPECTED_CAMERA_COUNT; cam++)
        {
            frames[cam] = cv::Mat();
            if(paths[cam].empty()) continue;
            frames[cam] = cv::imread(paths[cam], cv::IMREAD_COLOR_RGB);
            if(frames[cam].empty()) continue;
            loaded++;
        }
        if(loaded == 0) continue;

        const auto t0 = std::chrono::steady_clock::now();
        const bool ran = f_detector.run(frames, nullptr, result);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();

        if(!ran)
        {
            LOG_WARNING(SERVER_LOG_ID, "{}: inference did not run ({} images loaded)",
                        uuid, loaded);
            continue;
        }

        std::string darts;
        for(const DartDetection& d : result.newDarts)
        {
            char buf[64];
            std::snprintf(buf, sizeof(buf), " (angle %.1f r %.3f)",
                          static_cast<double>(d.angle),
                          static_cast<double>(d.normalizedRadius));
            darts += buf;
        }
        LOG_INFO(SERVER_LOG_ID, "{}: {} ms clear={}{} — {}",
                 uuid, ms, result.boardClear ? "Y" : "N",
                 darts.empty() ? std::string(" no new dart") : darts,
                 result.status);
    }

    f_detector.shutdown();
    shutdownWireCalibration();
    LOG_INFO(SERVER_LOG_ID, "replay complete");
    return STATUS_OK;
}
