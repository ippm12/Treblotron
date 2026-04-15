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
    constexpr float PEAK_THRESHOLD    = 0.65f;
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
    std::unique_ptr<VDevice>            vdevice;
    std::shared_ptr<InferModel>         inferModel;
    std::unique_ptr<ConfiguredInferModel> configuredInferModel;
    ConfiguredInferModel::Bindings      bindings;

    // Pre-allocated tensors reused across inferences.
    // Input: UINT8 NHWC — matches the HEF's native input format, so HailoRT
    // passes our camera bytes through without any quant/dequant work.
    // Output: FLOAT32 — HailoRT dequantizes the UINT8-quantized output into
    // real-domain logits for us.
    std::vector<uint8_t> inputTensor;   // INPUT_H * INPUT_W * INPUT_C
    std::vector<float>   outputTensor;  // OUTPUT_H * OUTPUT_W

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


// Helper — unwrap an Expected<T>, log on failure, return bool success so the
// init path stays flat. Using a macro so we can LOG_ERROR with the real status.
#define HAILO_CHECK(expected, what)                                                      \
    do {                                                                                 \
        if(!(expected)) {                                                                \
            LOG_ERROR(VISION_LOG_ID, what ": {}",                                        \
                      static_cast<int>((expected).status()));                            \
            return STATUS_ERROR_GENERIC;                                                 \
        }                                                                                \
    } while(0)


HailoVisionSource::~HailoVisionSource() = default;


Status HailoVisionSource::init()
{
    try
    {
    // Cameras must already be up so the inference thread can pull frames.
    initializeCameraSystem();

    LOG_INFO(VISION_LOG_ID, "HailoVisionSource: creating VDevice");
    auto vdevice_exp = VDevice::create();
    HAILO_CHECK(vdevice_exp, "VDevice::create failed");
    m_impl->vdevice = vdevice_exp.release();

    LOG_INFO(VISION_LOG_ID, "HailoVisionSource: creating InferModel from HEF");
    auto infer_model_exp = m_impl->vdevice->create_infer_model(HEF_PATH);
    HAILO_CHECK(infer_model_exp, "create_infer_model failed");
    m_impl->inferModel = infer_model_exp.release();

    // Match the HEF's native input format (UINT8) so HailoRT passes our camera
    // bytes through untouched, and request FLOAT32 output so HailoRT
    // dequantizes the heatmap into real-domain values for us.
    {
        auto input_exp = m_impl->inferModel->input();
        HAILO_CHECK(input_exp, "InferModel::input() failed");
        input_exp->set_format_type(HAILO_FORMAT_TYPE_UINT8);

        auto output_exp = m_impl->inferModel->output();
        HAILO_CHECK(output_exp, "InferModel::output() failed");
        output_exp->set_format_type(HAILO_FORMAT_TYPE_FLOAT32);
    }

    LOG_INFO(VISION_LOG_ID, "HailoVisionSource: configuring infer model");
    auto configured_exp = m_impl->inferModel->configure();
    HAILO_CHECK(configured_exp, "InferModel::configure failed");
    m_impl->configuredInferModel =
        std::make_unique<ConfiguredInferModel>(configured_exp.release());

    LOG_INFO(VISION_LOG_ID, "HailoVisionSource: creating bindings");
    auto bindings_exp = m_impl->configuredInferModel->create_bindings();
    HAILO_CHECK(bindings_exp, "create_bindings failed");
    m_impl->bindings = bindings_exp.release();

    // Point bindings at our pre-allocated input/output buffers — they're
    // reused every inference.
    {
        auto in_exp = m_impl->bindings.input();
        HAILO_CHECK(in_exp, "Bindings::input() failed");
        auto in_status = in_exp->set_buffer(MemoryView(
            m_impl->inputTensor.data(), m_impl->inputTensor.size()));
        if(in_status != HAILO_SUCCESS)
        {
            LOG_ERROR(VISION_LOG_ID, "input set_buffer failed: {}",
                      static_cast<int>(in_status));
            return STATUS_ERROR_GENERIC;
        }

        auto out_exp = m_impl->bindings.output();
        HAILO_CHECK(out_exp, "Bindings::output() failed");
        auto out_status = out_exp->set_buffer(MemoryView(
            m_impl->outputTensor.data(),
            m_impl->outputTensor.size() * sizeof(float)));
        if(out_status != HAILO_SUCCESS)
        {
            LOG_ERROR(VISION_LOG_ID, "output set_buffer failed: {}",
                      static_cast<int>(out_status));
            return STATUS_ERROR_GENERIC;
        }
    }

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

    // Tear down in reverse of init.
    m_impl->bindings = ConfiguredInferModel::Bindings();
    m_impl->configuredInferModel.reset();
    m_impl->inferModel.reset();
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

        // Pack into NHWC uint8. Pixels from missing cameras are zero.
        uint8_t* dst = m_impl->inputTensor.data();
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
                        dst[0] = px[0];
                        dst[1] = px[1];
                        dst[2] = px[2];
                    }
                    else
                    {
                        dst[0] = dst[1] = dst[2] = 0;
                    }
                    dst += 3;
                }
            }
        }

        // Synchronous inference — the bindings already point at our input/output
        // buffers, so run() reads from inputTensor and writes to outputTensor.
        auto run_status = m_impl->configuredInferModel->run(
            m_impl->bindings, std::chrono::milliseconds(1000));
        if(run_status != HAILO_SUCCESS)
        {
            LOG_WARNING(VISION_LOG_ID, "ConfiguredInferModel::run failed: {}",
                        static_cast<int>(run_status));
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
