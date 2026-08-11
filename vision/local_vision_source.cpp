/**
 * local_vision_source.cpp
 *
 * Camera plumbing and threading around DartDetector. Everything that used to
 * make this file 2000+ lines — engine building, preprocessing, decode, the
 * Detecting/Removing state machine — now lives in detect/, shared with the
 * inference server.
 */

#include "local_vision_source.hpp"
#include "detect/wire_calibration.hpp"
#include "vision/vision_settings.hpp"

#include <opencv2/core.hpp>

#include <chrono>
#include <exception>


namespace
{
    // The seg model's per-camera input plane: (3, 360, 640) floats. The camera
    // capture threads produce these in parallel with each other, so handing
    // them to the detector is strictly cheaper than letting it resize inline.
    constexpr size_t SEG_PER_CAM_FLOATS = 3u * 360u * 640u;

    /** Idle backoff when no camera has a usable frame yet. */
    constexpr int IDLE_SLEEP_MS = 50;
}


LocalVisionSource::LocalVisionSource() = default;


LocalVisionSource::~LocalVisionSource()
{
    // shutdown() is idempotent; call it here too so a source destroyed without
    // an explicit shutdown doesn't leave threads running into teardown.
    shutdown();
}


Status LocalVisionSource::init()
{
    try
    {
        // Start the camera system up front — it takes a moment too, and doing
        // it on the main thread keeps the threading model simple. The heavy
        // model load happens on a background thread below so the UI stays
        // interactive.
        initializeCameraSystem();

        {
            std::lock_guard<std::mutex> lock(m_buildStatusMutex);
            m_buildStatus = "Preparing";
        }
        m_buildProgress.store(0.0f, std::memory_order_release);
        m_buildState.store(BuildState::Building, std::memory_order_release);
        m_buildThread = std::thread(&LocalVisionSource::buildThreadMain, this);

        // The inference thread is spawned inside buildThreadMain() once the
        // detector is ready. init() returns immediately so the main render
        // loop can draw a loading screen.
        return STATUS_OK;
    }
    catch(const std::exception& e)
    {
        LOG_ERROR(VISION_LOG_ID, "LocalVisionSource::init threw: {}", e.what());
        m_buildState.store(BuildState::Failed, std::memory_order_release);
        return STATUS_ERROR_GENERIC;
    }
    catch(...)
    {
        LOG_ERROR(VISION_LOG_ID, "LocalVisionSource::init threw unknown exception");
        m_buildState.store(BuildState::Failed, std::memory_order_release);
        return STATUS_ERROR_GENERIC;
    }
}


void LocalVisionSource::buildThreadMain()
{
    auto setStatus = [&](const std::string& s)
    {
        std::lock_guard<std::mutex> lock(m_buildStatusMutex);
        m_buildStatus = s;
    };

    try
    {
        // Progress is monotonic across the whole startup — the bar fills 0 →
        // 100% once total, rather than bouncing back to 0 each time the
        // backend enters a new internal phase.
        auto onProgress = [&](float pct, uint64_t iter, const std::string& phase)
        {
            const float prev = m_buildProgress.load(std::memory_order_relaxed);
            if(pct > prev) m_buildProgress.store(pct, std::memory_order_release);
            if(iter) m_buildIteration.store(iter, std::memory_order_relaxed);
            if(!phase.empty()) setStatus(phase);
        };

        DartDetectorConfig config;
        config.tuning = getVisionSettings().tuning;
        m_tuningGeneration.store(getVisionSettingsGeneration(), std::memory_order_relaxed);

        const Status stat = m_detector.build(config, onProgress, m_buildAbort);

        if(m_buildAbort.load(std::memory_order_acquire))
        {
            setStatus("Aborted");
            m_buildState.store(BuildState::Failed, std::memory_order_release);
            return;
        }
        if(IS_STATUS_NOT_OK(stat))
        {
            setStatus("Failed: could not load detection models");
            m_buildState.store(BuildState::Failed, std::memory_order_release);
            return;
        }

        setStatus("Starting inference");
        m_running.store(true, std::memory_order_release);
        m_thread = std::thread(&LocalVisionSource::inferenceLoop, this);

        m_buildProgress.store(1.0f, std::memory_order_release);
        // Name the backend on the loading screen's last frame. A backend that
        // quietly fell back from the GPU is otherwise invisible until you
        // notice the frame rate, and that is worth knowing immediately.
        setStatus("Ready — " + m_detector.backendName());
        m_buildState.store(BuildState::Ready, std::memory_order_release);
        LOG_INFO(VISION_LOG_ID, "LocalVisionSource initialized on {}",
                 m_detector.backendName());
    }
    catch(const std::exception& e)
    {
        LOG_ERROR(VISION_LOG_ID, "buildThreadMain threw: {}", e.what());
        setStatus(std::string("Failed: ") + e.what());
        m_buildState.store(BuildState::Failed, std::memory_order_release);
    }
    catch(...)
    {
        LOG_ERROR(VISION_LOG_ID, "buildThreadMain threw unknown exception");
        setStatus("Failed: unknown exception");
        m_buildState.store(BuildState::Failed, std::memory_order_release);
    }
}


void LocalVisionSource::inferenceLoop()
{
    std::array<CameraFrame, EXPECTED_CAMERA_COUNT> cameraFrames;
    std::array<cv::Mat,     EXPECTED_CAMERA_COUNT> rawMats;
    DartDetectorResult result;

#ifdef DARTLENS_HAVE_LOCAL_INFERENCE
    // Scratch for the capture threads' pre-computed seg planes.
    std::array<std::vector<float>, EXPECTED_CAMERA_COUNT> segStorage;
    for(auto& s : segStorage) s.resize(SEG_PER_CAM_FLOATS);
    const float* segPlanes[EXPECTED_CAMERA_COUNT] = {};
#endif

    while(m_running.load(std::memory_order_acquire))
    {
        // Pick up a settings edit. One relaxed load per cycle in the common
        // case where nothing changed; the detector's own fields are atomics, so
        // applying mid-stream needs no coordination with the run() below.
        const uint32_t generation = getVisionSettingsGeneration();
        if(generation != m_tuningGeneration.load(std::memory_order_relaxed))
        {
            m_tuningGeneration.store(generation, std::memory_order_relaxed);
            m_detector.applyTuning(getVisionSettings().tuning);
        }

        if(m_resetRequested.exchange(false, std::memory_order_acq_rel))
        {
            m_detector.reset();
            m_boardClear.store(true, std::memory_order_release);
            std::lock_guard<std::mutex> lock(m_eventMutex);
            std::queue<std::pair<float, float>> empty;
            std::swap(m_newDartEvents, empty);
        }

        // Pull raw (unwarped) native-resolution frames from the capture
        // threads. The detector runs segmentation on the raw image and warps
        // the masked output itself.
        const uint32_t camCount = getCameraCount();
        uint32_t contributing = 0;
        for(uint32_t i = 0; i < EXPECTED_CAMERA_COUNT; i++)
        {
            rawMats[i] = cv::Mat();
            if(i >= camCount)              continue;
            if(!isCameraCalibrated(i))     continue;
            if(!getCameraFrame(i, cameraFrames[i])) continue;

            const CameraFrame& cf = cameraFrames[i];
            if(cf.pixels.empty()) continue;

            rawMats[i] = cv::Mat(cf.height, cf.width, CV_8UC3,
                                 const_cast<uint8_t*>(cf.pixels.data()),
                                 cf.stride);
            contributing++;
        }

        if(contributing == 0)
        {
            // Cameras not ready yet (or none calibrated) — idle briefly.
            std::this_thread::sleep_for(std::chrono::milliseconds(IDLE_SLEEP_MS));
            continue;
        }

#ifdef DARTLENS_HAVE_LOCAL_INFERENCE
        for(uint32_t i = 0; i < EXPECTED_CAMERA_COUNT; i++)
        {
            segPlanes[i] = (!rawMats[i].empty()
                         && getCameraSegPlane(i, segStorage[i].data(), SEG_PER_CAM_FLOATS))
                         ? segStorage[i].data() : nullptr;
        }
        const bool ran = m_detector.run(rawMats, segPlanes, result);
#else
        const bool ran = m_detector.run(rawMats, nullptr, result);
#endif

        if(!ran) continue;

        m_boardClear.store(result.boardClear, std::memory_order_release);

        {
            std::lock_guard<std::mutex> lock(m_statusMutex);
            m_detectionStatus = result.status;
        }

        // Hand the warped masked frames back to the camera system so
        // vision_debug (which calls getCameraWarpedFrame) gets a usable
        // preview without anyone paying for a second warp.
        for(uint32_t i = 0; i < EXPECTED_CAMERA_COUNT; i++)
        {
            const cv::Mat& w = m_detector.warpedFrame(i);
            if(w.empty()) continue;
            publishCameraWarpedFrame(i, w.ptr(), w.cols, w.rows,
                                     static_cast<int>(w.step[0]));
        }

        if(!result.newDarts.empty())
        {
            std::lock_guard<std::mutex> lock(m_eventMutex);
            for(const DartDetection& d : result.newDarts)
            {
                m_newDartEvents.emplace(d.angle, d.normalizedRadius);
            }
        }
    }
}


void LocalVisionSource::shutdown()
{
    // If the model build is still running, signal abort and wait for the
    // thread to unwind.
    m_buildAbort.store(true, std::memory_order_release);
    if(m_buildThread.joinable()) m_buildThread.join();

    // The build thread may have spawned the inference thread before seeing the
    // abort — explicitly stop it now.
    m_running.store(false, std::memory_order_release);
    if(m_thread.joinable()) m_thread.join();

    m_detector.shutdown();

    if(m_buildState.load(std::memory_order_acquire) != BuildState::Idle)
    {
        shutdownCameraSystem();
        m_buildState.store(BuildState::Idle, std::memory_order_release);
        LOG_INFO(VISION_LOG_ID, "LocalVisionSource shut down");
    }
}


// ============================================================================
// Per-frame hook — drains events queued by the inference thread
// ============================================================================

void LocalVisionSource::tick(float deltaTime)
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


bool LocalVisionSource::isBoardClear() const
{
    return m_boardClear.load(std::memory_order_acquire);
}


void LocalVisionSource::resetDarts()
{
    m_resetRequested.store(true, std::memory_order_release);
}


bool LocalVisionSource::getLatestHeatmap(std::vector<float>& out,
                                         uint32_t& width, uint32_t& height) const
{
    return m_detector.latestHeatmap(out, width, height);
}


bool LocalVisionSource::isInitializing() const
{
    return m_buildState.load(std::memory_order_acquire) == BuildState::Building;
}


bool LocalVisionSource::isFailed() const
{
    return m_buildState.load(std::memory_order_acquire) == BuildState::Failed;
}


float LocalVisionSource::getInitProgress() const
{
    return m_buildProgress.load(std::memory_order_acquire);
}


uint64_t LocalVisionSource::getInitIteration() const
{
    return m_buildIteration.load(std::memory_order_relaxed);
}


std::string LocalVisionSource::getInitStatus() const
{
    std::lock_guard<std::mutex> lock(m_buildStatusMutex);
    return m_buildStatus;
}


std::string LocalVisionSource::getDetectionStatus() const
{
    std::lock_guard<std::mutex> lock(m_statusMutex);
    return m_detectionStatus;
}
