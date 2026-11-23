/**
 * vision_debug.cpp
 *
 * Vision debug screen implementation.
 */

#include "vision_debug.hpp"
#include "vision/vision.hpp"
#include "detect/wire_calibration.hpp"
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
#include <SDL3_ttf/SDL_ttf.h>

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

static constexpr float    WINDOW_W       = 1920.0f;
static constexpr float    WINDOW_H       = 1080.0f;

static constexpr float    TITLE_PT       = 72.0f;
static constexpr float    BODY_PT        = 28.0f;

static constexpr float    LEFT_MARGIN    = 60.0f;
static constexpr float    TITLE_Y        = 22.0f;
static constexpr uint32_t BASE_Z         = 10;

// Keep both stages in one streaming texture so they update together.
using Preview = DartPreview;
static constexpr int PANEL_GAP = 12;
static constexpr int BOARD_X = Preview::cameraWidth + PANEL_GAP;
static constexpr int ATLAS_W = BOARD_X + Preview::boardSize;
static constexpr float PREVIEW_SCALE = 1.8f;
static constexpr float PREVIEW_Y = 200.0f;

// Dartboard (right side)
static constexpr float    BOARD_CENTER_X = 1570.0f;
static constexpr float    BOARD_CENTER_Y = 380.0f;
static constexpr float    BOARD_SCALE    = 0.95f;

// Dart list (below board on right side)
static constexpr float    DART_LIST_X    = 1360.0f;
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

#ifdef TREBLOTRON_USE_SIM
    // In sim builds there are no cameras or heatmap to debug — the screen
    // is meaningless. Skip the heavy resource setup and let render() show a
    // "not available" message that any input dismisses.
    m_simMode = true;
    return STATUS_OK;
#endif

    // Create dartboard on right side
    m_board = DartBoard::create(m_world, BOARD_CENTER_X, BOARD_CENTER_Y,
                                BOARD_SCALE, m_bodyFontId, BASE_Z);

    SDL_Renderer* renderer = getFrameRenderer(frameId);
    if(!renderer) return STATUS_ERROR_GENERIC;
    m_compositeTexture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB24,
        SDL_TEXTUREACCESS_STREAMING, ATLAS_W, Preview::boardSize);
    if(!m_compositeTexture) return STATUS_ERROR_GENERIC;
    setVisionPreviewEnabled(true);
    return STATUS_OK;
}


// ============================================================================
// Display the latest complete inference. Both views share its publication time.
void VisionDebugScreen::updateComposite()
{
    std::chrono::steady_clock::time_point published;
    auto frame = getVisionPreview(published);
    if(!frame || frame == m_displayedPreview) return;
    VISION_PROFILE_SCOPE(m_timings, "preview");
    cv::Mat atlas(Preview::boardSize, ATLAS_W, CV_8UC3, cv::Scalar(0,0,0));
    auto camera = atlas(cv::Rect(0,0,Preview::cameraWidth,Preview::cameraHeight));
    auto board = atlas(cv::Rect(BOARD_X,0,Preview::boardSize,Preview::boardSize));

    cv::Mat rgb(Preview::cameraHeight, Preview::cameraWidth, CV_8UC3,
                const_cast<uint8_t*>(frame->rgb.data()));
    rgb.copyTo(camera);
    cv::Mat mask(camera.size(), CV_8UC1);
    for(int i=0; i<Preview::cameraPixels; ++i)
        mask.data[i] = (frame->maskBits[i/8] & (1u << (i%8))) ? 255 : 0;
    cv::Mat edge;
    cv::morphologyEx(mask, edge, cv::MORPH_GRADIENT, cv::Mat());
    camera.setTo(cv::Scalar(0,255,255), edge);

    for(int y=0; y<board.rows; ++y)
    {
        auto* row = board.ptr<cv::Vec3b>(y);
        for(int x=0; x<board.cols; ++x)
        {
            const int i = y*board.cols + x;
            const int heat = frame->heat[i], gray = frame->boardGray[i];
            const auto base = static_cast<uint8_t>(gray * (255-heat) / 255);
            row[x] = cv::Vec3b(base+heat,base,base);
        }
    }
    for(size_t i=0; i+1<frame->tips.size(); i+=2)
        cv::circle(board, {frame->tips[i],frame->tips[i+1]}, 4, cv::Scalar(225,100,255), 1);
    if(SDL_UpdateTexture(m_compositeTexture,nullptr,atlas.data,static_cast<int>(atlas.step)))
    {
        m_displayedPreview = std::move(frame);
        m_displayedHeatmapTime = published;
    }
}

void VisionDebugScreen::update(float deltaTime)
{
    (void)deltaTime;

    if(m_simMode) return;

    {
        VISION_PROFILE_SCOPE(m_timings, "update");

    const auto captureResult = consumeVisionCaptureResult();
    if(!captureResult.empty()) m_captureStatus = captureResult;
    const auto stats = getVisionPreviewStats();
    updateComposite();

    const double now = SDL_GetTicksNS() / 1e9;
    if(m_rateWindowStart == 0)
    {
        m_rateWindowStart = now;
        m_initialDetectionCount = stats.detectionCount;
    }
    else ++m_uiFrames;
    const double elapsed = now - m_rateWindowStart;
    if(elapsed >= 1.0)
    {
        char rates[96];
        std::snprintf(rates, sizeof(rates), "UI %.0f FPS    Detector %.1f Hz",
                      m_uiFrames / elapsed, (stats.detectionCount - m_initialDetectionCount) / elapsed);
        m_rateLabel = rates;
        m_uiFrames = 0;
        m_initialDetectionCount = stats.detectionCount;
        m_rateWindowStart = now;
    }

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

    if(m_simMode)
    {
        TTF_Font* font = getFont(m_bodyFontId);

        auto centerLine = [&](const std::string& s, float y, Color c) {
            int w = 0, h = 0;
            if(font) TTF_GetStringSize(font, s.c_str(), 0, &w, &h);
            auto text = std::make_shared<RenderText>();
            text->m_text     = s;
            text->m_color    = c;
            text->m_fontId   = m_bodyFontId;
            text->m_rotation = 0.0f;
            text->m_scaleX   = 1.0f;
            text->m_scaleY   = 1.0f;
            text->m_x        = (WINDOW_W - static_cast<float>(w)) * 0.5f;
            text->m_y        = y;
            text->m_z        = BASE_Z;
            renderQueueAdd(fid, text);
        };

        centerLine("Vision Debug is not available in simulation mode.",
                   WINDOW_H * 0.5f - 30.0f, {220, 220, 230});
        centerLine("Run a build with real cameras to use this screen.",
                   WINDOW_H * 0.5f + 10.0f, {150, 150, 160});

        std::vector<InputHint> hints;
        hints.push_back({SDLK_ESCAPE, SDL_GAMEPAD_BUTTON_EAST, "back"});
        m_inputHints.render(fid, m_bodyFontId, LEFT_MARGIN, HINTS_Y, BASE_Z, hints);
        return;
    }

    auto label = [&](const std::string& value,float x,float y) {
        auto text = std::make_shared<RenderText>();
        text->m_text = value;
        text->m_fontId = m_bodyFontId;
        text->m_x = x; text->m_y = y; text->m_z = BASE_Z;
        text->m_scaleX = text->m_scaleY = 1;
        text->m_color = {215,220,225};
        renderQueueAdd(fid,text);
    };
    if(m_displayedPreview)
    {
        label("Camera " + std::to_string(1+m_displayedPreview->camera) + "   /   Segmentation",
              LEFT_MARGIN,PREVIEW_Y-40);
        label("Board   /   Tip heatmap",LEFT_MARGIN+BOARD_X*PREVIEW_SCALE,PREVIEW_Y-40);
        auto tex = std::make_shared<RenderCachedTexture>();
        tex->m_texture = m_compositeTexture;
        tex->m_width = ATLAS_W * PREVIEW_SCALE;
        tex->m_height = Preview::boardSize * PREVIEW_SCALE;
        tex->m_x = LEFT_MARGIN;
        tex->m_y = PREVIEW_Y;
        tex->m_z = BASE_Z;
        renderQueueAdd(fid, tex);
        label("Frame " + std::to_string(m_displayedPreview->sequence),
              LEFT_MARGIN,PREVIEW_Y+Preview::cameraHeight*PREVIEW_SCALE+8);
    }
    else label("Waiting for a calibrated camera and detector preview",LEFT_MARGIN,PREVIEW_Y);

    if(!m_captureStatus.empty())
        label(m_captureStatus, LEFT_MARGIN, PREVIEW_Y+Preview::cameraHeight*PREVIEW_SCALE+43);

    // Detection-state badge — rendered AFTER the composite image so it
    // lands on top (the render queue draws in insertion order, not by
    // m_z, so position in the queue is what matters). Colored by mode:
    // green = idle Detecting, yellow = entering Removing, orange =
    // currently Removing. Dark backdrop guarantees readability against
    // any segment color or heatmap intensity behind it.
    {
        std::string status = getVisionDetectionStatus();
        if(!status.empty())
        {
            Color color = {180, 180, 190};
            if(status.rfind("Detecting (hand", 0) == 0) color = {240, 220, 100};
            else if(status.rfind("Detecting", 0) == 0)     color = {120, 220, 140};
            else if(status.rfind("Removing", 0) == 0)      color = {240, 130, 90};

            const float textX = LEFT_MARGIN;
            const float textY = TITLE_Y + 80.0f;

            TTF_Font* font = getFont(m_bodyFontId);
            if(font)
            {
                int textW = 0, textH = 0;
                if(TTF_GetStringSize(font, status.c_str(), 0, &textW, &textH))
                {
                    constexpr float padX = 10.0f;
                    constexpr float padY = 4.0f;
                    auto bg = std::make_shared<RenderShape>();
                    bg->m_type   = ShapeType::Box;
                    bg->m_color  = {20, 24, 32};
                    bg->m_x      = textX - padX;
                    bg->m_y      = textY - padY;
                    bg->m_width  = static_cast<float>(textW) + 2.0f * padX;
                    bg->m_height = static_cast<float>(textH) + 2.0f * padY;
                    bg->m_z      = BASE_Z;
                    renderQueueAdd(fid, bg);
                }
            }

            auto text = std::make_shared<RenderText>();
            text->m_text     = status;
            text->m_color    = color;
            text->m_fontId   = m_bodyFontId;
            text->m_rotation = 0.0f;
            text->m_scaleX   = 1.0f;
            text->m_scaleY   = 1.0f;
            text->m_x        = textX;
            text->m_y        = textY;
            text->m_z        = BASE_Z;
            renderQueueAdd(fid, text);
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
        float ty = 615.0f;
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
            text->m_x        = LEFT_MARGIN;
            text->m_y        = ty;
            text->m_z        = BASE_Z + 4;
            renderQueueAdd(fid, text);
            ty += 28.0f;
        }
    }

    {
        std::string heatmapAge = "Waiting for heatmap";
        if(m_displayedHeatmapTime != std::chrono::steady_clock::time_point{})
        {
            const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - m_displayedHeatmapTime).count();
            heatmapAge = "Heatmap age " + std::to_string(age) + " ms";
        }
        auto text = std::make_shared<RenderText>();
        text->m_text = m_rateLabel + "    " + heatmapAge;
        text->m_color = {200, 200, 210};
        text->m_fontId = m_bodyFontId;
        text->m_scaleX = text->m_scaleY = 1.0f;
        // Keep the readout in the title row, above the shared game bar.
        text->m_x = 620.0f;
        text->m_y = TITLE_Y + 20.0f;
        text->m_z = BASE_Z;
        renderQueueAdd(fid, text);
    }

    // Input hints
    std::vector<InputHint> hints;
    hints.push_back({SDLK_ESCAPE, SDL_GAMEPAD_BUTTON_EAST, "back"});
    hints.push_back({SDLK_R, SDL_GAMEPAD_BUTTON_WEST, "reset darts"});
    if(m_displayedPreview)
    {
        hints.push_back({SDLK_C, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, "next camera"});
        hints.push_back({SDLK_M, SDL_GAMEPAD_BUTTON_NORTH, "mark missed dart"});
    }
    m_inputHints.render(fid, m_bodyFontId, LEFT_MARGIN, HINTS_Y, BASE_Z, hints);
}


void VisionDebugScreen::shutdown()
{
    setVisionPreviewEnabled(false);
    m_displayedPreview.reset();
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
    if(m_simMode)
    {
        // Any key dismisses the unavailable-message screen.
        (void)keycode;
        loadGame(std::make_shared<MainMenu>());
        return;
    }

    switch(keycode)
    {
        case SDLK_ESCAPE:
            loadGame(std::make_shared<MainMenu>());
            break;
        case SDLK_C:
            if(m_displayedPreview)
                setVisionPreviewEnabled(true, (m_displayedPreview->camera + 1) % EXPECTED_CAMERA_COUNT);
            break;
        case SDLK_M:
            if(m_displayedPreview)
            {
                const auto seq = m_displayedPreview->sequence;
                if(markMissedVisionDart(*m_displayedPreview))
                    m_captureStatus = "Saving missed dart on frame " + std::to_string(seq);
                else m_captureStatus = "Capture unavailable or already pending";
            }
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

    if(m_simMode)
    {
        // Any button dismisses the unavailable-message screen.
        (void)button;
        loadGame(std::make_shared<MainMenu>());
        return;
    }

    switch(button)
    {
        case SDL_GAMEPAD_BUTTON_EAST:  // B
            onKeyDown(SDLK_ESCAPE);
            break;
        case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER:
            onKeyDown(SDLK_C);
            break;
        case SDL_GAMEPAD_BUTTON_NORTH:
            onKeyDown(SDLK_M);
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
