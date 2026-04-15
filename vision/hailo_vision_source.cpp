/**
 * hailo_vision_source.cpp
 *
 * HailoRT-backed dart detection. Grabs the latest frame from each camera
 * every inference, warps it into the 720x720 template space, stacks the
 * three RGB images into a single 9-channel tensor, runs the U-Net on the
 * HAILO device, and decodes the output heatmap into dart events.
 *
 * This file is only compiled when DARTLENS_USE_HAILO is enabled; the
 * Windows sim build skips it entirely, so HailoRT headers only need to be
 * available on the Pi.
 */

#include "hailo_vision_source.hpp"
#include "vision/vision.hpp"
#include "vision/wire_calibration.hpp"
#include "peak_detection.hpp"
#include "dart/dart_board_geometry.hpp"

#include <hailo/hailort.hpp>

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

#include <chrono>
#include <cmath>
#include <cstring>

using namespace hailort;


// ============================================================================
// Constants
// ============================================================================

namespace
{
    // Path relative to the executable — the .hef ships alongside other assets.
    constexpr const char* HEF_PATH = "./vision/multicam_unet/multicam_unet.hef";

    // Model IO shapes (must match the ONNX export used to compile the .hef).
    constexpr uint32_t INPUT_W   = 720;
    constexpr uint32_t INPUT_H   = 720;
    constexpr uint32_t INPUT_C   = 9;  // 3 cameras x 3 RGB channels
    constexpr uint32_t OUTPUT_W  = 180;
    constexpr uint32_t OUTPUT_H  = 180;
    constexpr uint32_t N_CAMS    = 3;

    // Peak-detection parameters — match DartModelTraining/heatmap_utils.py.
    constexpr float PEAK_THRESHOLD    = 0.55f;
    constexpr int   PEAK_MIN_DISTANCE = 2;

    // A new peak within this distance (in heatmap pixels) of a previous-frame
    // peak is treated as the same dart, not a new throw. 4px @180 ≈ 16px @720
    // ≈ 9.4mm on a standard board, well inside any reasonable jitter.
    constexpr float DART_MATCH_RADIUS_PX = 4.0f;

    // Template-space geometry — matches wire_calibration.cpp and the
    // DartModelTraining board_detection.py layout.
    constexpr float TEMPLATE_CENTER = 360.0f;
    constexpr float BOARD_RADIUS_PX = 290.0f;

    // Heatmap → template scale (720 / 180 = 4)
    constexpr float HEATMAP_TO_TEMPLATE = static_cast<float>(INPUT_W) / OUTPUT_W;
}


// ============================================================================
// PImpl — holds HailoRT handles so the header stays SDK-free
// ============================================================================

struct HailoVisionSource::Impl
{
    std::unique_ptr<VDevice>                 vdevice;
    std::shared_ptr<ConfiguredNetworkGroup>  networkGroup;
    std::vector<InputVStream>                inputStreams;
    std::vector<OutputVStream>               outputStreams;
    std::unique_ptr<ActivatedNetworkGroup>   activated;

    // Pre-allocated tensors reused across inferences.
    std::vector<float> inputTensor;   // NHWC: H*W*C
    std::vector<float> outputTensor;  // H*W

    // Warp destinations for each camera (720x720 RGB8, recycled each frame).
    cv::Mat warped[N_CAMS];

    // Latest camera frames copied out of the capture system.
    CameraFrame cameraFrames[N_CAMS];
};


// ============================================================================
// Lifecycle
// ============================================================================

HailoVisionSource::HailoVisionSource() : m_impl(std::make_unique<Impl>())
{
    m_impl->inputTensor.resize(static_cast<size_t>(INPUT_W) * INPUT_H * INPUT_C);
    m_impl->outputTensor.resize(static_cast<size_t>(OUTPUT_W) * OUTPUT_H);
}


HailoVisionSource::~HailoVisionSource() = default;


Status HailoVisionSource::init()
{
    try
    {
    // Cameras must already be up so the inference thread can pull frames.
    initializeCameraSystem();

    LOG_INFO(VISION_LOG_ID, "HailoVisionSource: creating VDevice");
    auto vdevice_exp = VDevice::create();
    if(!vdevice_exp)
    {
        LOG_ERROR(VISION_LOG_ID, "VDevice::create failed: {}", static_cast<int>(vdevice_exp.status()));
        return STATUS_ERROR_GENERIC;
    }
    m_impl->vdevice = vdevice_exp.release();

    LOG_INFO(VISION_LOG_ID, "HailoVisionSource: loading HEF");
    auto hef_exp = Hef::create(HEF_PATH);
    if(!hef_exp)
    {
        LOG_ERROR(VISION_LOG_ID, "Failed to load HEF '{}': {}", HEF_PATH, static_cast<int>(hef_exp.status()));
        return STATUS_ERROR_GENERIC;
    }

    LOG_INFO(VISION_LOG_ID, "HailoVisionSource: creating configure params");
    auto configure_params_exp = hef_exp->create_configure_params(HAILO_STREAM_INTERFACE_PCIE);
    if(!configure_params_exp)
    {
        LOG_ERROR(VISION_LOG_ID, "create_configure_params failed: {}", static_cast<int>(configure_params_exp.status()));
        return STATUS_ERROR_GENERIC;
    }

    LOG_INFO(VISION_LOG_ID, "HailoVisionSource: configuring network groups");
    auto network_groups_exp = m_impl->vdevice->configure(hef_exp.value(), configure_params_exp.value());
    if(!network_groups_exp)
    {
        LOG_ERROR(VISION_LOG_ID, "VDevice::configure failed: {}", static_cast<int>(network_groups_exp.status()));
        return STATUS_ERROR_GENERIC;
    }
    if(network_groups_exp->empty())
    {
        LOG_ERROR(VISION_LOG_ID, "VDevice::configure returned no network groups");
        return STATUS_ERROR_GENERIC;
    }
    m_impl->networkGroup = network_groups_exp.value()[0];

    LOG_INFO(VISION_LOG_ID, "HailoVisionSource: building vstream params");
    // Ask for float32 IO — HailoRT handles quant/dequant for us.
    auto input_params_exp = m_impl->networkGroup->make_input_vstream_params(
        {}, HAILO_FORMAT_TYPE_FLOAT32,
        HAILO_DEFAULT_VSTREAM_TIMEOUT_MS, HAILO_DEFAULT_VSTREAM_QUEUE_SIZE);
    auto output_params_exp = m_impl->networkGroup->make_output_vstream_params(
        {}, HAILO_FORMAT_TYPE_FLOAT32,
        HAILO_DEFAULT_VSTREAM_TIMEOUT_MS, HAILO_DEFAULT_VSTREAM_QUEUE_SIZE);
    if(!input_params_exp || !output_params_exp)
    {
        LOG_ERROR(VISION_LOG_ID, "make_*_vstream_params failed");
        return STATUS_ERROR_GENERIC;
    }

    auto input_streams_exp = VStreamsBuilder::create_input_vstreams(
        *m_impl->networkGroup, input_params_exp.value());
    auto output_streams_exp = VStreamsBuilder::create_output_vstreams(
        *m_impl->networkGroup, output_params_exp.value());
    if(!input_streams_exp || !output_streams_exp)
    {
        LOG_ERROR(VISION_LOG_ID, "create_*_vstreams failed");
        return STATUS_ERROR_GENERIC;
    }
    m_impl->inputStreams  = input_streams_exp.release();
    m_impl->outputStreams = output_streams_exp.release();

    LOG_INFO(VISION_LOG_ID, "HailoVisionSource: activating network group");
    auto activated_exp = m_impl->networkGroup->activate();
    if(!activated_exp)
    {
        LOG_ERROR(VISION_LOG_ID, "activate failed: {}", static_cast<int>(activated_exp.status()));
        return STATUS_ERROR_GENERIC;
    }
    m_impl->activated = activated_exp.release();

    m_running.store(true, std::memory_order_release);
    m_thread = std::thread(&HailoVisionSource::inferenceLoop, this);

    LOG_INFO(VISION_LOG_ID, "HailoVisionSource initialized ({})", HEF_PATH);
    return STATUS_OK;
    }
    catch(const std::exception& e)
    {
        LOG_ERROR(VISION_LOG_ID, "HailoVisionSource::init threw: {}", e.what());
        return STATUS_ERROR_GENERIC;
    }
    catch(...)
    {
        LOG_ERROR(VISION_LOG_ID, "HailoVisionSource::init threw unknown exception");
        return STATUS_ERROR_GENERIC;
    }
}


void HailoVisionSource::shutdown()
{
    m_running.store(false, std::memory_order_release);
    if(m_thread.joinable())
    {
        m_thread.join();
    }

    // Tear down in reverse of init — activated scope, streams, network, device.
    m_impl->activated.reset();
    m_impl->outputStreams.clear();
    m_impl->inputStreams.clear();
    m_impl->networkGroup.reset();
    m_impl->vdevice.reset();

    shutdownCameraSystem();
    LOG_INFO(VISION_LOG_ID, "HailoVisionSource shut down");
}


// ============================================================================
// Per-frame hook — drains events queued by the inference thread
// ============================================================================

void HailoVisionSource::tick(float deltaTime)
{
    (void)deltaTime;

    std::queue<PolarDart> drained;
    {
        std::lock_guard<std::mutex> lock(m_eventMutex);
        std::swap(drained, m_newDartEvents);
    }

    while(!drained.empty())
    {
        const PolarDart& d = drained.front();
        if(m_onDartLanded) m_onDartLanded();
        if(m_onDartPositionCalculated) m_onDartPositionCalculated(d.angle, d.normalizedRadius);
        drained.pop();
    }
}


bool HailoVisionSource::isBoardClear() const
{
    return m_boardClear.load(std::memory_order_acquire);
}


void HailoVisionSource::resetDarts()
{
    m_resetRequested.store(true, std::memory_order_release);
}


bool HailoVisionSource::getLatestHeatmap(std::vector<float>& out,
                                         uint32_t& width, uint32_t& height) const
{
    std::lock_guard<std::mutex> lock(m_heatmapMutex);
    if(m_latestHeatmap.empty()) return false;
    out    = m_latestHeatmap;
    width  = m_latestHeatmapW;
    height = m_latestHeatmapH;
    return true;
}


// ============================================================================
// Inference loop (runs on m_thread)
// ============================================================================

void HailoVisionSource::inferenceLoop()
{
    while(m_running.load(std::memory_order_acquire))
    {
        if(m_resetRequested.exchange(false, std::memory_order_acq_rel))
        {
            m_prevPeaks.clear();
            m_boardClear.store(true, std::memory_order_release);
            {
                std::lock_guard<std::mutex> lock(m_eventMutex);
                std::queue<PolarDart> empty;
                std::swap(m_newDartEvents, empty);
            }
        }

        // Build input tensor — warp each camera into template space and
        // interleave their RGB channels into NHWC order.
        uint32_t camCount = getCameraCount();
        uint32_t contributing = 0;
        for(uint32_t i = 0; i < N_CAMS; i++)
        {
            m_impl->warped[i] = cv::Mat();  // reset
            if(i >= camCount) continue;
            if(!isCameraCalibrated(i)) continue;
            if(!getCameraFrame(i, m_impl->cameraFrames[i])) continue;

            const CameraFrame& cf = m_impl->cameraFrames[i];
            if(cf.pixels.empty()) continue;

            cv::Mat src(cf.height, cf.width, CV_8UC3,
                        const_cast<uint8_t*>(cf.pixels.data()), cf.stride);

            if(!warpCameraFrame(i, src, m_impl->warped[i])) continue;
            if(m_impl->warped[i].empty()) continue;
            contributing++;
        }

        if(contributing == 0)
        {
            // Not enough calibrated cameras yet — idle briefly.
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }

        // Pack into NHWC float32 [0,1]. Pixels from missing cameras are zero.
        float* dst = m_impl->inputTensor.data();
        for(uint32_t r = 0; r < INPUT_H; r++)
        {
            for(uint32_t c = 0; c < INPUT_W; c++)
            {
                for(uint32_t cam = 0; cam < N_CAMS; cam++)
                {
                    const cv::Mat& w = m_impl->warped[cam];
                    if(!w.empty())
                    {
                        const uint8_t* px = w.ptr(r) + c * 3;
                        dst[0] = px[0] / 255.0f;
                        dst[1] = px[1] / 255.0f;
                        dst[2] = px[2] / 255.0f;
                    }
                    else
                    {
                        dst[0] = dst[1] = dst[2] = 0.0f;
                    }
                    dst += 3;
                }
            }
        }

        // Inference — single input stream, single output stream.
        auto write_status = m_impl->inputStreams[0].write(
            MemoryView(m_impl->inputTensor.data(),
                       m_impl->inputTensor.size() * sizeof(float)));
        if(write_status != HAILO_SUCCESS)
        {
            LOG_WARNING(VISION_LOG_ID, "vstream write failed: {}", static_cast<int>(write_status));
            continue;
        }

        auto read_status = m_impl->outputStreams[0].read(
            MemoryView(m_impl->outputTensor.data(),
                       m_impl->outputTensor.size() * sizeof(float)));
        if(read_status != HAILO_SUCCESS)
        {
            LOG_WARNING(VISION_LOG_ID, "vstream read failed: {}", static_cast<int>(read_status));
            continue;
        }

        // Apply sigmoid — training exported logits, so we squash them here
        // rather than baking the sigmoid into the exported graph.
        for(float& v : m_impl->outputTensor)
        {
            v = 1.0f / (1.0f + std::exp(-v));
        }

        // Publish a snapshot for the debug UI before running peak detection,
        // so the overlay and the reported dart events always come from the
        // same inference.
        {
            std::lock_guard<std::mutex> lock(m_heatmapMutex);
            m_latestHeatmap  = m_impl->outputTensor;
            m_latestHeatmapW = OUTPUT_W;
            m_latestHeatmapH = OUTPUT_H;
        }

        handleHeatmap(m_impl->outputTensor.data(), OUTPUT_W, OUTPUT_H);
    }
}


// ============================================================================
// Peak decode + dart tracking
// ============================================================================

void HailoVisionSource::handleHeatmap(const float* heatmap, uint32_t w, uint32_t h)
{
    auto peaks = Vision::findHeatmapPeaks(heatmap, w, h,
                                          PEAK_THRESHOLD, PEAK_MIN_DISTANCE);

    // Convert heatmap peaks → polar (angle, normalized radius).
    std::vector<PolarDart> current;
    current.reserve(peaks.size());
    for(const auto& p : peaks)
    {
        float tx = p.col * HEATMAP_TO_TEMPLATE;
        float ty = p.row * HEATMAP_TO_TEMPLATE;
        float dx = tx - TEMPLATE_CENTER;
        float dy = ty - TEMPLATE_CENTER;

        float dist = std::sqrt(dx * dx + dy * dy);
        float normR = dist / BOARD_RADIUS_PX;
        if(normR > DartBoardGeometry::RADIUS_DOUBLE_OUTER) continue;

        float angle = static_cast<float>(std::atan2(dy, dx) * 180.0 / M_PI);

        current.push_back({angle, normR});
    }

    m_boardClear.store(current.empty(), std::memory_order_release);

    // Match each current peak to the previous-frame set (in heatmap space for
    // a distance threshold that's invariant to angle). Any unmatched current
    // peak is treated as a new dart and emitted as an event.
    const float matchR2 = (DART_MATCH_RADIUS_PX * DART_MATCH_RADIUS_PX) *
                          (HEATMAP_TO_TEMPLATE * HEATMAP_TO_TEMPLATE);

    std::vector<PolarDart> newDarts;
    for(const PolarDart& curr : current)
    {
        float crx = std::cos(curr.angle * M_PI / 180.0f) *
                    curr.normalizedRadius * BOARD_RADIUS_PX;
        float cry = std::sin(curr.angle * M_PI / 180.0f) *
                    curr.normalizedRadius * BOARD_RADIUS_PX;

        bool matched = false;
        for(const PolarDart& prev : m_prevPeaks)
        {
            float prx = std::cos(prev.angle * M_PI / 180.0f) *
                        prev.normalizedRadius * BOARD_RADIUS_PX;
            float pry = std::sin(prev.angle * M_PI / 180.0f) *
                        prev.normalizedRadius * BOARD_RADIUS_PX;
            float ex = crx - prx;
            float ey = cry - pry;
            if(ex * ex + ey * ey <= matchR2)
            {
                matched = true;
                break;
            }
        }

        if(!matched)
        {
            newDarts.push_back(curr);
        }
    }

    m_prevPeaks = std::move(current);

    if(!newDarts.empty())
    {
        std::lock_guard<std::mutex> lock(m_eventMutex);
        for(const PolarDart& d : newDarts)
        {
            m_newDartEvents.push(d);
        }
    }
}
