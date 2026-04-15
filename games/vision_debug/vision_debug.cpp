/**
 * vision_debug.cpp
 *
 * Vision debug screen implementation.
 */

#include "vision_debug.hpp"
#include "vision/vision.hpp"
#include "vision/wire_calibration.hpp"
#include "game_lib/game_manager.hpp"
#include "game_lib/game_helpers.hpp"
#include "games/main_menu.hpp"
#include "game_lib/components/render_shape.hpp"
#include "game_lib/components/render_text.hpp"
#include "game_lib/components/render_cached_texture.hpp"
#include "frame/frame.hpp"
#include "frame/render_queue.hpp"

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

#include <SDL3/SDL.h>
#include <SDL3/SDL_keycode.h>
#include <SDL3/SDL_gamepad.h>

#include "debug/common_logging.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// Template-space geometry — mirror of wire_calibration.cpp / board_detection.py
static constexpr float TEMPLATE_CENTER = 360.0f;
static constexpr float BOARD_RADIUS_PX = 290.0f;
static constexpr float DART_MARKER_PX  = 7.0f;


// ============================================================================
// Layout constants
// ============================================================================

static constexpr float    WINDOW_W       = 1920.0f;
static constexpr float    WINDOW_H       = 1080.0f;

static constexpr float    TITLE_PT       = 72.0f;
static constexpr float    BODY_PT        = 28.0f;

static constexpr float    LEFT_MARGIN    = 60.0f;
static constexpr float    TITLE_Y        = 22.0f;
static constexpr uint32_t BASE_Z         = 10;

// AI model stub image (left side)
static constexpr float    AI_IMAGE_X     = LEFT_MARGIN;
static constexpr float    AI_IMAGE_Y     = 120.0f;
static constexpr float    AI_IMAGE_SIZE  = 720.0f;

// Dartboard (right side)
static constexpr float    BOARD_CENTER_X = 1300.0f;
static constexpr float    BOARD_CENTER_Y = 380.0f;
static constexpr float    BOARD_SCALE    = 1.2f;

// Dart list (below board on right side)
static constexpr float    DART_LIST_X    = 1060.0f;
static constexpr float    DART_LIST_Y    = 650.0f;
static constexpr float    DART_LIST_LINE = 32.0f;
static constexpr size_t   DART_LIST_MAX  = 12;

// Input hints
static constexpr float    HINTS_Y        = 862.0f;


// ============================================================================
// Construction
// ============================================================================

VisionDebugScreen::VisionDebugScreen()
    : Game("Vision Debug")
    , m_titleFontId(INVALID_FONT_ID)
    , m_bodyFontId(INVALID_FONT_ID)
{
}


// ============================================================================
// Lifecycle
// ============================================================================

Status VisionDebugScreen::init(FrameID frameId)
{
    m_frameId = frameId;

    m_titleFontId = loadFont("assets/fonts/Roboto-Regular.ttf", TITLE_PT);
    if(m_titleFontId == INVALID_FONT_ID)
    {
        return STATUS_ERROR_GENERIC;
    }

    m_bodyFontId = loadFont("assets/fonts/Roboto-Regular.ttf", BODY_PT);
    if(m_bodyFontId == INVALID_FONT_ID)
    {
        return STATUS_ERROR_GENERIC;
    }

    m_inputHints.init();

    // Create dartboard on right side
    m_board = DartBoard::create(m_world, BOARD_CENTER_X, BOARD_CENTER_Y,
                                BOARD_SCALE, m_bodyFontId, BASE_Z);

    // Streaming 720x720 RGB texture, updated every frame with the alpha-blended
    // composite of the warped camera frames
    SDL_Renderer* rend = getFrameRenderer(frameId);
    if(rend)
    {
        m_compositeTexture = SDL_CreateTexture(rend, SDL_PIXELFORMAT_RGB24,
                                               SDL_TEXTUREACCESS_STREAMING,
                                               static_cast<int>(WARPED_OUTPUT_SIZE),
                                               static_cast<int>(WARPED_OUTPUT_SIZE));
    }
    m_compositeBuffer.assign(WARPED_OUTPUT_SIZE * WARPED_OUTPUT_SIZE * 3, 40);

    return STATUS_OK;
}


// ============================================================================
// Composite update — warp each calibrated camera into the 720x720 template
// space and average them together
// ============================================================================

void VisionDebugScreen::updateComposite()
{
    VISION_PROFILE_SCOPE(m_timings, "composite");

    const int W = static_cast<int>(WARPED_OUTPUT_SIZE);
    const int H = static_cast<int>(WARPED_OUTPUT_SIZE);

    cv::Mat accumulator(H, W, CV_32FC3, cv::Scalar(0, 0, 0));
    int contributing = 0;

    {
        VISION_PROFILE_SCOPE(m_timings, "getFrames");
        uint32_t count = getCameraCount();
        for(uint32_t i = 0; i < count; i++)
        {
            if(!isCameraCalibrated(i)) continue;
            getCameraFrame(i, m_cameraFrames[i]);
        }
    }

    {
        VISION_PROFILE_SCOPE(m_timings, "warp");
        uint32_t count = getCameraCount();
        for(uint32_t i = 0; i < count; i++)
        {
            if(!isCameraCalibrated(i)) continue;

            const CameraFrame& cf = m_cameraFrames[i];
            if(cf.pixels.empty() || cf.width <= 0 || cf.height <= 0) continue;

            cv::Mat src(cf.height, cf.width, CV_8UC3,
                        const_cast<uint8_t*>(cf.pixels.data()), cf.stride);

            cv::Mat warped;
            if(!warpCameraFrame(i, src, warped) || warped.empty()) continue;

            cv::Mat warpedF;
            warped.convertTo(warpedF, CV_32FC3);
            accumulator += warpedF;
            contributing++;
        }
    }

    cv::Mat out(H, W, CV_8UC3, cv::Scalar(0, 0, 0));
    {
        VISION_PROFILE_SCOPE(m_timings, "avg");
        if(contributing > 0)
        {
            accumulator /= static_cast<float>(contributing);
            accumulator.convertTo(out, CV_8UC3);
        }
    }

    // Overlay the dart detection heatmap (if the active vision source
    // produces one) as a red tint on top of the warped composite. The
    // heatmap is in template space so it lines up 1:1 with the 720x720
    // buffer after upscaling.
    std::vector<float> heatmap;
    uint32_t hmW = 0, hmH = 0;
    bool haveHeatmap = false;
    {
        VISION_PROFILE_SCOPE(m_timings, "heatmapFetch");
        haveHeatmap = getLatestVisionHeatmap(heatmap, hmW, hmH) && hmW > 0 && hmH > 0;
    }
    if(haveHeatmap)
    {
        VISION_PROFILE_SCOPE(m_timings, "heatmapOverlay");
        const float scaleX = static_cast<float>(hmW) / W;
        const float scaleY = static_cast<float>(hmH) / H;
        for(int y = 0; y < H; y++)
        {
            int hy = static_cast<int>(y * scaleY);
            if(hy >= static_cast<int>(hmH)) hy = hmH - 1;
            const float* hrow = heatmap.data() + static_cast<size_t>(hy) * hmW;
            uint8_t* orow = out.ptr(y);
            for(int x = 0; x < W; x++)
            {
                int hx = static_cast<int>(x * scaleX);
                if(hx >= static_cast<int>(hmW)) hx = hmW - 1;
                float v = std::clamp(hrow[hx], 0.0f, 1.0f);
                int boost = static_cast<int>(v * 255.0f);
                int r = orow[x * 3 + 0] + boost;
                orow[x * 3 + 0] = static_cast<uint8_t>(std::min(r, 255));
            }
        }
    }

    {
        VISION_PROFILE_SCOPE(m_timings, "texUpload");
        // Copy to contiguous buffer (row by row, respecting stride)
        if(m_compositeBuffer.size() != static_cast<size_t>(W) * H * 3)
        {
            m_compositeBuffer.resize(static_cast<size_t>(W) * H * 3);
        }
        for(int y = 0; y < H; y++)
        {
            std::memcpy(m_compositeBuffer.data() + static_cast<size_t>(y) * W * 3,
                        out.ptr(y), static_cast<size_t>(W) * 3);
        }

        if(m_compositeTexture)
        {
            SDL_UpdateTexture(m_compositeTexture, nullptr,
                              m_compositeBuffer.data(), W * 3);
        }
    }
}


void VisionDebugScreen::update(float deltaTime)
{
    (void)deltaTime;

    {
        VISION_PROFILE_SCOPE(m_timings, "update");

    updateComposite();

    // Mirror the vision source's "board cleared" transition so tracked darts
    // clear when the user hits collect (sim) or the real system resets.
    bool boardClear = isBoardClear();
    if(boardClear && !m_lastBoardClear)
    {
        m_trackedDarts.clear();
        m_hitPositions.clear();
        m_board.unhighlightAll();
    }
    m_lastBoardClear = boardClear;

    // Consume any dart events from the vision system
    consumeDartLandedCount();

    DartPosition pos;
    while(popDartPosition(pos))
    {
        auto seg = polarToSegment(pos.angle, pos.normalizedRadius);
        if(seg.has_value())
        {
            TrackedDart td;
            td.segment          = seg.value();
            td.angle            = pos.angle;
            td.normalizedRadius = pos.normalizedRadius;
            m_trackedDarts.push_back(td);
            m_hitPositions.push_back(pos);

            m_board.highlightSegment(seg.value());
        }
    }
    }  // end "update" scope

    m_timings.nextFrame();

    double nowSec = static_cast<double>(SDL_GetTicksNS()) / 1e9;
    if(m_lastLogSec == 0.0) m_lastLogSec = nowSec;
    if(nowSec - m_lastLogSec >= 2.0)
    {
        m_lastLogSec = nowSec;
        std::string line;
        for(const auto& e : m_timings.snapshot())
        {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%s=%.1f ", e.name, e.avgMs);
            line += buf;
        }
        LOG_INFO(VISION_LOG_ID, "vision_debug timings: {}", line);
    }
}


void VisionDebugScreen::render()
{
    VISION_PROFILE_SCOPE(m_timings, "render");
    FrameID fid = getFrameId();

    // Title
    {
        auto text = std::make_shared<RenderText>();
        text->m_text     = "Vision Debug";
        text->m_color    = {255, 255, 255};
        text->m_fontId   = m_titleFontId;
        text->m_rotation = 0.0f;
        text->m_scaleX   = 1.0f;
        text->m_scaleY   = 1.0f;
        text->m_x        = LEFT_MARGIN;
        text->m_y        = TITLE_Y;
        text->m_z        = BASE_Z;
        renderQueueAdd(fid, text);
    }

    // Warped composite image (alpha-blended from calibrated cameras)
    if(m_compositeTexture)
    {
        auto tex = std::make_shared<RenderCachedTexture>();
        tex->m_texture = m_compositeTexture;
        tex->m_width   = AI_IMAGE_SIZE;
        tex->m_height  = AI_IMAGE_SIZE;
        tex->m_x       = AI_IMAGE_X;
        tex->m_y       = AI_IMAGE_Y;
        tex->m_z       = BASE_Z;
        renderQueueAdd(fid, tex);
    }

    // Dart markers over the composite image — map each tracked polar hit into
    // template space (720x720), then into composite-image screen space.
    if(m_compositeTexture)
    {
        for(const DartPosition& d : m_hitPositions)
        {
            float rad = d.angle * static_cast<float>(M_PI) / 180.0f;
            float tx = TEMPLATE_CENTER + std::cos(rad) * d.normalizedRadius * BOARD_RADIUS_PX;
            float ty = TEMPLATE_CENTER + std::sin(rad) * d.normalizedRadius * BOARD_RADIUS_PX;
            float sx = AI_IMAGE_X + tx / WARPED_OUTPUT_SIZE * AI_IMAGE_SIZE;
            float sy = AI_IMAGE_Y + ty / WARPED_OUTPUT_SIZE * AI_IMAGE_SIZE;

            auto marker = std::make_shared<RenderShape>();
            marker->m_type  = ShapeType::Circle;
            marker->m_x     = sx;
            marker->m_y     = sy;
            marker->m_width = DART_MARKER_PX * 2.0f;
            marker->m_color = {255, 255, 0};
            marker->m_z     = BASE_Z + 2;
            renderQueueAdd(fid, marker);
        }
    }

    // Dartboard
    m_board.enqueueRender(fid);
    renderHitMarkers(fid, m_hitPositions, BOARD_CENTER_X, BOARD_CENTER_Y, BOARD_SCALE);

    // Dart list
    {
        // Header
        auto header = std::make_shared<RenderText>();
        header->m_text     = "Tracked Darts";
        header->m_color    = {200, 200, 200};
        header->m_fontId   = m_bodyFontId;
        header->m_rotation = 0.0f;
        header->m_scaleX   = 1.0f;
        header->m_scaleY   = 1.0f;
        header->m_x        = DART_LIST_X;
        header->m_y        = DART_LIST_Y;
        header->m_z        = BASE_Z;
        renderQueueAdd(fid, header);

        // Show most recent darts (scroll to bottom)
        size_t startIdx = 0;
        if(m_trackedDarts.size() > DART_LIST_MAX)
        {
            startIdx = m_trackedDarts.size() - DART_LIST_MAX;
        }

        float y = DART_LIST_Y + DART_LIST_LINE + 4.0f;
        for(size_t i = startIdx; i < m_trackedDarts.size(); i++)
        {
            const TrackedDart& td = m_trackedDarts[i];

            std::string line = std::to_string(i + 1) + ". "
                + getSegmentName(td.segment)
                + "  (a:" + std::to_string(static_cast<int>(td.angle))
                + " r:" + std::to_string(td.normalizedRadius).substr(0, 4) + ")";

            auto text = std::make_shared<RenderText>();
            text->m_text     = line;
            text->m_color    = {180, 180, 180};
            text->m_fontId   = m_bodyFontId;
            text->m_rotation = 0.0f;
            text->m_scaleX   = 1.0f;
            text->m_scaleY   = 1.0f;
            text->m_x        = DART_LIST_X;
            text->m_y        = y;
            text->m_z        = BASE_Z;
            renderQueueAdd(fid, text);

            y += DART_LIST_LINE;
        }

        if(m_trackedDarts.empty())
        {
            auto empty = std::make_shared<RenderText>();
            empty->m_text     = "No darts detected";
            empty->m_color    = {100, 100, 100};
            empty->m_fontId   = m_bodyFontId;
            empty->m_rotation = 0.0f;
            empty->m_scaleX   = 1.0f;
            empty->m_scaleY   = 1.0f;
            empty->m_x        = DART_LIST_X;
            empty->m_y        = y;
            empty->m_z        = BASE_Z;
            renderQueueAdd(fid, empty);
        }
    }

    // Timing overlay — top-right corner, updated live
    {
        VISION_PROFILE_SCOPE(m_timings, "timingOverlay");
        float ty = 80.0f;
        for(const auto& e : m_timings.snapshot())
        {
            char buf[80];
            std::snprintf(buf, sizeof(buf), "%-14s %5.1f / %5.1f ms",
                          e.name, e.lastMs, e.avgMs);

            auto text = std::make_shared<RenderText>();
            text->m_text     = buf;
            text->m_color    = {140, 140, 150};
            text->m_fontId   = m_bodyFontId;
            text->m_rotation = 0.0f;
            text->m_scaleX   = 1.0f;
            text->m_scaleY   = 1.0f;
            text->m_x        = WINDOW_W - 340.0f;
            text->m_y        = ty;
            text->m_z        = BASE_Z + 4;
            renderQueueAdd(fid, text);
            ty += 28.0f;
        }
    }

    // Input hints
    std::vector<InputHint> hints;
    hints.push_back({SDLK_ESCAPE, SDL_GAMEPAD_BUTTON_EAST, "back"});
    hints.push_back({SDLK_R, SDL_GAMEPAD_BUTTON_WEST, "reset darts"});
    m_inputHints.render(fid, m_bodyFontId, LEFT_MARGIN, HINTS_Y, BASE_Z, hints);
}


void VisionDebugScreen::shutdown()
{
    if(m_compositeTexture)
    {
        SDL_DestroyTexture(m_compositeTexture);
        m_compositeTexture = nullptr;
    }

    m_inputHints.shutdown();

    if(m_bodyFontId != INVALID_FONT_ID)
    {
        unloadFont(m_bodyFontId);
        m_bodyFontId = INVALID_FONT_ID;
    }
    if(m_titleFontId != INVALID_FONT_ID)
    {
        unloadFont(m_titleFontId);
        m_titleFontId = INVALID_FONT_ID;
    }
}


bool VisionDebugScreen::isPauseable() const
{
    return false;
}


// ============================================================================
// Input
// ============================================================================

void VisionDebugScreen::onKeyDown(uint32_t keycode)
{
    switch(keycode)
    {
        case SDLK_ESCAPE:
            loadGame(std::make_shared<MainMenu>());
            break;
        case SDLK_R:
            resetDarts();
            break;
        default:
            break;
    }
}


void VisionDebugScreen::onGamepadButton(uint8_t button, bool pressed)
{
    if(!pressed) return;

    switch(button)
    {
        case SDL_GAMEPAD_BUTTON_EAST:  // B
            onKeyDown(SDLK_ESCAPE);
            break;
        case SDL_GAMEPAD_BUTTON_WEST:  // X
            onKeyDown(SDLK_R);
            break;
        default:
            break;
    }
}


// ============================================================================
// Dart reset
// ============================================================================

void VisionDebugScreen::resetDarts()
{
    resetVisionDarts();
    m_trackedDarts.clear();
    m_hitPositions.clear();
    m_board.unhighlightAll();
}
