/**
 * calibration.cpp
 *
 * Camera calibration screen implementation.
 */

#include "calibration.hpp"
#include "vision/vision.hpp"
#include "detect/wire_calibration.hpp"
#include "game_lib/game_manager.hpp"
#include "games/main_menu.hpp"
#include "game_lib/components/render_shape.hpp"
#include "game_lib/components/render_text.hpp"
#include "game_lib/components/render_cached_texture.hpp"
#include "frame/frame.hpp"
#include "frame/render_queue.hpp"

#include <SDL3/SDL.h>
#include <SDL3/SDL_keycode.h>
#include <SDL3/SDL_gamepad.h>
#include <SDL3_ttf/SDL_ttf.h>

#include <algorithm>
#include <cmath>
#include <memory>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif


// ============================================================================
// Layout constants
// ============================================================================

static constexpr float    WINDOW_W       = 1920.0f;
static constexpr float    WINDOW_H       = 1080.0f;

static constexpr float    TITLE_PT       = 72.0f;
static constexpr float    BODY_PT        = 39.0f;
static constexpr float    GUIDE_PT       = 28.0f;
static constexpr float    SMALL_PT       = 22.0f;

static constexpr float    LEFT_MARGIN    = 60.0f;
static constexpr float    TITLE_Y        = 22.0f;
static constexpr uint32_t BASE_Z         = 10;

// Overview: 3-column slot grid
static constexpr float    SLOT_TOP       = 120.0f;
static constexpr float    SLOT_GAP       = 30.0f;
static constexpr float    SLOT_W         = (WINDOW_W - LEFT_MARGIN * 2.0f - SLOT_GAP * 2.0f) / 3.0f;
static constexpr float    SLOT_H         = 630.0f;
static constexpr float    SLOT_LABEL_H   = 60.0f;
static constexpr float    SLOT_IMG_PAD   = 15.0f;
static constexpr float    FOCUS_BORDER   = 4.0f;

// Calibrate: camera view on the left, target guide panel on the right
static constexpr float    CAL_VIEW_TOP   = 150.0f;
static constexpr float    CAL_VIEW_H     = 690.0f;
static constexpr float    CAL_VIEW_MAXW  = 1160.0f;

static constexpr float    GUIDE_X        = 1290.0f;
static constexpr float    GUIDE_W        = WINDOW_W - GUIDE_X - LEFT_MARGIN;
static constexpr float    GUIDE_PAD      = 14.0f;

// Board diagram inside the guide panel
static constexpr float    DIAGRAM_RADIUS = 172.0f;
static constexpr float    NUMBER_RATIO   = 1.17f;   // where bed numbers sit
static constexpr float    MARKER_RADIUS  = 11.0f;

// Placed point marker
static constexpr float    POINT_RADIUS   = 6.0f;

// Status message
static constexpr float    STATUS_Y       = 780.0f;
static constexpr float    STATUS_FADE    = 3.0f;

// Input hints
static constexpr float    HINTS_Y        = 862.0f;


// ============================================================================
// Construction
// ============================================================================

CalibrationScreen::CalibrationScreen()
    : Game("Calibration")
    , m_titleFontId(INVALID_FONT_ID)
    , m_bodyFontId(INVALID_FONT_ID)
    , m_guideFontId(INVALID_FONT_ID)
    , m_smallFontId(INVALID_FONT_ID)
    , m_cameraCount(0)
    , m_statusTimer(0.0f)
{
}


// ============================================================================
// Lifecycle
// ============================================================================

Status CalibrationScreen::init(FrameID frameId)
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

    m_guideFontId = loadFont("assets/fonts/Roboto-Regular.ttf", GUIDE_PT);
    if(m_guideFontId == INVALID_FONT_ID)
    {
        return STATUS_ERROR_GENERIC;
    }

    m_smallFontId = loadFont("assets/fonts/Roboto-Regular.ttf", SMALL_PT);
    if(m_smallFontId == INVALID_FONT_ID)
    {
        return STATUS_ERROR_GENERIC;
    }

    m_inputHints.init();

    initializeCameraSystem();
    return STATUS_OK;
}


void CalibrationScreen::update(float deltaTime)
{
    m_cameraCount = getCameraCount();

    if(m_statusTimer > 0.0f)
    {
        m_statusTimer -= deltaTime;
    }
}


void CalibrationScreen::render()
{
    FrameID fid = getFrameId();

    // Title
    const char* titleText = (m_mode == Mode::Overview) ? "Calibration" : "Wire Calibration";
    {
        auto text = std::make_shared<RenderText>();
        text->m_text     = titleText;
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

    if(m_mode == Mode::Overview)
    {
        renderOverview();
    }
    else
    {
        renderCalibrate();
    }

    // Status message
    if(m_statusTimer > 0.0f && !m_statusMessage.empty())
    {
        TTF_Font* bodyFont = getFont(m_bodyFontId);
        int textW = 0, textH = 0;
        if(bodyFont)
        {
            TTF_GetStringSize(bodyFont, m_statusMessage.c_str(), 0, &textW, &textH);
        }
        auto text = std::make_shared<RenderText>();
        text->m_text     = m_statusMessage;
        text->m_color    = {100, 220, 100};
        text->m_fontId   = m_bodyFontId;
        text->m_rotation = 0.0f;
        text->m_scaleX   = 1.0f;
        text->m_scaleY   = 1.0f;
        text->m_x        = (WINDOW_W - static_cast<float>(textW)) * 0.5f;
        text->m_y        = STATUS_Y;
        text->m_z        = BASE_Z + 5;
        renderQueueAdd(fid, text);
    }

    // Input hints
    std::vector<InputHint> hints;
    if(m_mode == Mode::Overview)
    {
        hints.push_back({SDLK_ESCAPE, SDL_GAMEPAD_BUTTON_EAST, "back"});
        hints.push_back({SDLK_RETURN, SDL_GAMEPAD_BUTTON_SOUTH, "calibrate"});
        hints.push_back({SDLK_S, SDL_GAMEPAD_BUTTON_WEST, "save"});
        hints.push_back({SDLK_LEFTBRACKET,  SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,  "swap left"});
        hints.push_back({SDLK_RIGHTBRACKET, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, "swap right"});
#ifndef NDEBUG
        hints.push_back({SDLK_F, SDL_GAMEPAD_BUTTON_NORTH, "save frames"});
#endif
    }
    else
    {
        hints.push_back({SDLK_ESCAPE, SDL_GAMEPAD_BUTTON_EAST, "back"});
        hints.push_back({SDLK_C, SDL_GAMEPAD_BUTTON_WEST, "clear"});
    }
    m_inputHints.render(fid, m_bodyFontId, LEFT_MARGIN, HINTS_Y, BASE_Z, hints);
}


void CalibrationScreen::shutdown()
{
    for(uint32_t i = 0; i < EXPECTED_CAMERA_COUNT; i++)
    {
        if(m_cameraTextures[i])
        {
            SDL_DestroyTexture(m_cameraTextures[i]);
            m_cameraTextures[i] = nullptr;
        }
    }

    shutdownCameraSystem();
    m_inputHints.shutdown();

    if(m_smallFontId != INVALID_FONT_ID)
    {
        unloadFont(m_smallFontId);
        m_smallFontId = INVALID_FONT_ID;
    }
    if(m_guideFontId != INVALID_FONT_ID)
    {
        unloadFont(m_guideFontId);
        m_guideFontId = INVALID_FONT_ID;
    }
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


bool CalibrationScreen::isPauseable() const
{
    return false;
}


// ============================================================================
// Input
// ============================================================================

void CalibrationScreen::onKeyDown(uint32_t keycode)
{
    if(m_mode == Mode::Overview)
    {
        switch(keycode)
        {
            case SDLK_ESCAPE:
                loadGame(std::make_shared<MainMenu>());
                break;
            case SDLK_LEFT:
                if(m_focusedSlot > 0) m_focusedSlot--;
                break;
            case SDLK_RIGHT:
                if(m_focusedSlot + 1 < EXPECTED_CAMERA_COUNT) m_focusedSlot++;
                break;
            case SDLK_RETURN:
            case SDLK_KP_ENTER:
                if(m_focusedSlot < m_cameraCount)
                {
                    m_calibrateCam = m_focusedSlot;
                    m_mode = Mode::Calibrate;
                }
                break;
            case SDLK_S:
            {
                Status st = saveWireCalibration();
                showStatus(IS_STATUS_OK(st) ? "Calibration saved" : "Save failed");
                break;
            }
            case SDLK_LEFTBRACKET:
                if(m_focusedSlot > 0 && swapCameraSlots(m_focusedSlot, m_focusedSlot - 1))
                {
                    // The texture at each slot is a snapshot of the previous
                    // physical camera — drop them so the next render fetches
                    // the new slot's actual frame.
                    for(uint32_t i = 0; i < EXPECTED_CAMERA_COUNT; i++)
                    {
                        if(m_cameraTextures[i])
                        {
                            SDL_DestroyTexture(m_cameraTextures[i]);
                            m_cameraTextures[i] = nullptr;
                        }
                    }
                    m_focusedSlot--;
                    showStatus("Swapped");
                }
                break;
            case SDLK_RIGHTBRACKET:
                if(m_focusedSlot + 1 < m_cameraCount &&
                   swapCameraSlots(m_focusedSlot, m_focusedSlot + 1))
                {
                    for(uint32_t i = 0; i < EXPECTED_CAMERA_COUNT; i++)
                    {
                        if(m_cameraTextures[i])
                        {
                            SDL_DestroyTexture(m_cameraTextures[i]);
                            m_cameraTextures[i] = nullptr;
                        }
                    }
                    m_focusedSlot++;
                    showStatus("Swapped");
                }
                break;
#ifndef NDEBUG
            case SDLK_F:
                saveFrames();
                break;
#endif
            default:
                break;
        }
    }
    else  // Calibrate mode
    {
        switch(keycode)
        {
            case SDLK_ESCAPE:
                m_mode = Mode::Overview;
                break;
            case SDLK_C:
                clearWirePoints(m_calibrateCam);
                break;
            default:
                break;
        }
    }
}


void CalibrationScreen::onGamepadButton(uint8_t button, bool pressed)
{
    if(!pressed) return;

    if(m_mode == Mode::Overview)
    {
        switch(button)
        {
            case SDL_GAMEPAD_BUTTON_EAST:          onKeyDown(SDLK_ESCAPE); break;
            case SDL_GAMEPAD_BUTTON_SOUTH:         onKeyDown(SDLK_RETURN); break;
            case SDL_GAMEPAD_BUTTON_DPAD_LEFT:     onKeyDown(SDLK_LEFT);   break;
            case SDL_GAMEPAD_BUTTON_DPAD_RIGHT:    onKeyDown(SDLK_RIGHT);  break;
            case SDL_GAMEPAD_BUTTON_WEST:          onKeyDown(SDLK_S);      break;
            case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER:  onKeyDown(SDLK_LEFTBRACKET);  break;
            case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: onKeyDown(SDLK_RIGHTBRACKET); break;
#ifndef NDEBUG
            case SDL_GAMEPAD_BUTTON_NORTH:         onKeyDown(SDLK_F);      break;
#endif
            default: break;
        }
    }
    else
    {
        switch(button)
        {
            case SDL_GAMEPAD_BUTTON_EAST: onKeyDown(SDLK_ESCAPE); break;
            case SDL_GAMEPAD_BUTTON_WEST: onKeyDown(SDLK_C);      break;
            default: break;
        }
    }
}


void CalibrationScreen::onMouseClick(float x, float y, uint8_t button)
{
    if(m_mode == Mode::Overview)
    {
        if(button != 1) return;
        for(uint32_t i = 0; i < EXPECTED_CAMERA_COUNT; i++)
        {
            if(i < m_cameraCount && m_slotDrawRects[i].contains(x, y))
            {
                m_focusedSlot = i;
                m_calibrateCam = i;
                m_mode = Mode::Calibrate;
                return;
            }
        }
    }
    else  // Calibrate mode
    {
        float fx = 0, fy = 0;
        if(!screenToFrame(m_calibrateCam, m_calibrateDrawRect, x, y, fx, fy))
        {
            return;
        }

        if(button == 1)
        {
            addWirePoint(m_calibrateCam, fx, fy);
        }
        else if(button == 3)
        {
            undoLastWirePoint(m_calibrateCam);
        }
    }
}


// ============================================================================
// Helpers
// ============================================================================

void CalibrationScreen::drawText(const std::string& text, FontID fontId, Color color,
                                 float x, float y, uint32_t z, bool centred)
{
    if(text.empty()) return;

    float drawX = x;
    if(centred)
    {
        TTF_Font* font = getFont(fontId);
        int textW = 0, textH = 0;
        if(font)
        {
            TTF_GetStringSize(font, text.c_str(), 0, &textW, &textH);
        }
        drawX = x - static_cast<float>(textW) * 0.5f;
    }

    auto obj = std::make_shared<RenderText>();
    obj->m_text     = text;
    obj->m_color    = color;
    obj->m_fontId   = fontId;
    obj->m_rotation = 0.0f;
    obj->m_scaleX   = 1.0f;
    obj->m_scaleY   = 1.0f;
    obj->m_x        = drawX;
    obj->m_y        = y;
    obj->m_z        = z;
    renderQueueAdd(getFrameId(), obj);
}


void CalibrationScreen::drawDot(float x, float y, float radius, Color color, uint32_t z)
{
    auto dot = std::make_shared<RenderShape>();
    dot->m_type   = ShapeType::Circle;
    dot->m_color  = color;
    dot->m_x      = x;
    dot->m_y      = y;
    dot->m_z      = z;
    dot->m_width  = radius * 2.0f;
    dot->m_height = 0.0f;
    renderQueueAdd(getFrameId(), dot);
}


void CalibrationScreen::showStatus(const std::string& msg)
{
    m_statusMessage = msg;
    m_statusTimer = STATUS_FADE;
}


bool CalibrationScreen::screenToFrame(uint32_t camIndex, const Rect& draw,
                                      float sx, float sy, float& outX, float& outY) const
{
    if(draw.w <= 0.0f || draw.h <= 0.0f) return false;
    if(!draw.contains(sx, sy)) return false;
    if(m_lastFrameW[camIndex] <= 0.0f || m_lastFrameH[camIndex] <= 0.0f) return false;

    outX = (sx - draw.x) / draw.w * m_lastFrameW[camIndex];
    outY = (sy - draw.y) / draw.h * m_lastFrameH[camIndex];
    return true;
}


void CalibrationScreen::updateCameraTexture(uint32_t index)
{
    if(index >= m_cameraCount) return;

    if(getCameraFrame(index, m_cameraFrames[index]))
    {
        if(m_cameraTextures[index])
        {
            SDL_DestroyTexture(m_cameraTextures[index]);
            m_cameraTextures[index] = nullptr;
        }

        SDL_Surface* surface = SDL_CreateSurfaceFrom(
            m_cameraFrames[index].width, m_cameraFrames[index].height,
            SDL_PIXELFORMAT_RGB24,
            m_cameraFrames[index].pixels.data(),
            m_cameraFrames[index].stride);

        if(surface)
        {
            m_cameraTextures[index] = SDL_CreateTextureFromSurface(
                getFrameRenderer(getFrameId()), surface);
            SDL_DestroySurface(surface);
        }

        m_lastFrameW[index] = static_cast<float>(m_cameraFrames[index].width);
        m_lastFrameH[index] = static_cast<float>(m_cameraFrames[index].height);
    }
}


// ============================================================================
// Overview rendering
// ============================================================================

void CalibrationScreen::renderOverview()
{
    for(uint32_t i = 0; i < EXPECTED_CAMERA_COUNT; i++)
    {
        float slotX = LEFT_MARGIN + static_cast<float>(i) * (SLOT_W + SLOT_GAP);
        float slotY = SLOT_TOP;
        renderSlot(i, slotX, slotY);
    }
}


void CalibrationScreen::renderSlot(uint32_t index, float slotX, float slotY)
{
    FrameID fid = getFrameId();

    // Focus border (draw behind bg as 4 thin strips around)
    if(index == m_focusedSlot)
    {
        Color focusColor = {120, 200, 255};
        auto makeStrip = [&](float x, float y, float w, float h)
        {
            auto s = std::make_shared<RenderShape>();
            s->m_type   = ShapeType::Box;
            s->m_color  = focusColor;
            s->m_x      = x;
            s->m_y      = y;
            s->m_z      = BASE_Z;
            s->m_width  = w;
            s->m_height = h;
            renderQueueAdd(fid, s);
        };
        makeStrip(slotX - FOCUS_BORDER, slotY - FOCUS_BORDER,
                  SLOT_W + FOCUS_BORDER * 2.0f, FOCUS_BORDER);
        makeStrip(slotX - FOCUS_BORDER, slotY + SLOT_H,
                  SLOT_W + FOCUS_BORDER * 2.0f, FOCUS_BORDER);
        makeStrip(slotX - FOCUS_BORDER, slotY, FOCUS_BORDER, SLOT_H);
        makeStrip(slotX + SLOT_W,       slotY, FOCUS_BORDER, SLOT_H);
    }

    // Slot background
    auto bg = std::make_shared<RenderShape>();
    bg->m_type   = ShapeType::Box;
    bg->m_color  = {40, 40, 48};
    bg->m_x      = slotX;
    bg->m_y      = slotY;
    bg->m_z      = BASE_Z + 1;
    bg->m_width  = SLOT_W;
    bg->m_height = SLOT_H;
    renderQueueAdd(fid, bg);

    // Name + calibration counter
    std::string name = getCameraName(index);
    if(index < m_cameraCount)
    {
        uint32_t placed = getWirePointCount(index);
        name += "  (" + std::to_string(placed) + "/" +
                std::to_string(WIRE_POINTS_PER_CAMERA) + ")";
        if(isCameraCalibrated(index))
        {
            name += " OK";
        }
    }

    auto nameText = std::make_shared<RenderText>();
    nameText->m_text     = name;
    nameText->m_color    = {180, 180, 190};
    nameText->m_fontId   = m_bodyFontId;
    nameText->m_rotation = 0.0f;
    nameText->m_scaleX   = 1.0f;
    nameText->m_scaleY   = 1.0f;
    nameText->m_x        = slotX + 22.0f;
    nameText->m_y        = slotY + 15.0f;
    nameText->m_z        = BASE_Z + 2;
    renderQueueAdd(fid, nameText);

    // Reset draw rect
    m_slotDrawRects[index] = Rect{};

    bool hasFrame = false;
    std::string statusStr;
    Color statusColor = {200, 80, 80};

    if(index < m_cameraCount)
    {
        updateCameraTexture(index);

        if(m_cameraTextures[index])
        {
            hasFrame = true;

            float availW = SLOT_W - SLOT_IMG_PAD * 2.0f;
            float availH = SLOT_H - SLOT_LABEL_H - SLOT_IMG_PAD;
            float scale = std::min(availW / m_lastFrameW[index], availH / m_lastFrameH[index]);
            float drawW = m_lastFrameW[index] * scale;
            float drawH = m_lastFrameH[index] * scale;
            float drawX = slotX + (SLOT_W - drawW) * 0.5f;
            float drawY = slotY + SLOT_LABEL_H + (availH - drawH) * 0.5f;

            m_slotDrawRects[index] = {drawX, drawY, drawW, drawH};

            auto tex = std::make_shared<RenderCachedTexture>();
            tex->m_texture = m_cameraTextures[index];
            tex->m_x      = drawX;
            tex->m_y      = drawY;
            tex->m_z      = BASE_Z + 2;
            tex->m_width  = drawW;
            tex->m_height = drawH;
            renderQueueAdd(fid, tex);
        }
        else
        {
            statusStr = "No feed";
            statusColor = {200, 200, 80};
        }
    }
    else
    {
        statusStr = "Camera not found";
    }

    if(!hasFrame)
    {
        TTF_Font* bodyFont = getFont(m_bodyFontId);
        int textW = 0, textH = 0;
        if(bodyFont)
        {
            TTF_GetStringSize(bodyFont, statusStr.c_str(), 0, &textW, &textH);
        }
        auto statusText = std::make_shared<RenderText>();
        statusText->m_text     = statusStr;
        statusText->m_color    = statusColor;
        statusText->m_fontId   = m_bodyFontId;
        statusText->m_rotation = 0.0f;
        statusText->m_scaleX   = 1.0f;
        statusText->m_scaleY   = 1.0f;
        statusText->m_x        = slotX + (SLOT_W - static_cast<float>(textW)) * 0.5f;
        statusText->m_y        = slotY + (SLOT_H - static_cast<float>(textH)) * 0.5f;
        statusText->m_z        = BASE_Z + 2;
        renderQueueAdd(fid, statusText);
    }
}


// ============================================================================
// Calibrate rendering
// ============================================================================

void CalibrationScreen::renderCalibrate()
{
    FrameID fid = getFrameId();

    updateCameraTexture(m_calibrateCam);

    m_calibrateDrawRect = Rect{};

    uint32_t placed = getWirePointCount(m_calibrateCam);

    // Camera label + progress, under the title
    {
        std::string header = getCameraName(m_calibrateCam);
        header += isCameraCalibrated(m_calibrateCam)
                    ? "   -   calibrated, all 40 points placed"
                    : "   -   point " + std::to_string(placed + 1) + " of " +
                      std::to_string(WIRE_POINTS_PER_CAMERA);
        drawText(header, m_bodyFontId, {220, 220, 230},
                 LEFT_MARGIN, TITLE_Y + TITLE_PT + 8.0f, BASE_Z + 2);
    }

    renderTargetGuide(placed);

    if(!m_cameraTextures[m_calibrateCam] ||
       m_lastFrameW[m_calibrateCam] <= 0.0f || m_lastFrameH[m_calibrateCam] <= 0.0f)
    {
        drawText("No frame available", m_bodyFontId, {200, 200, 80},
                 LEFT_MARGIN, CAL_VIEW_TOP + CAL_VIEW_H * 0.5f, BASE_Z + 2);
        return;
    }

    // Camera view, fitted into the space left of the guide panel
    float frameW = m_lastFrameW[m_calibrateCam];
    float frameH = m_lastFrameH[m_calibrateCam];
    float scale  = std::min(CAL_VIEW_MAXW / frameW, CAL_VIEW_H / frameH);
    float drawW  = frameW * scale;
    float drawH  = frameH * scale;
    float drawX  = LEFT_MARGIN + (CAL_VIEW_MAXW - drawW) * 0.5f;
    float drawY  = CAL_VIEW_TOP + (CAL_VIEW_H - drawH) * 0.5f;

    m_calibrateDrawRect = {drawX, drawY, drawW, drawH};

    {
        auto tex = std::make_shared<RenderCachedTexture>();
        tex->m_texture = m_cameraTextures[m_calibrateCam];
        tex->m_x      = drawX;
        tex->m_y      = drawY;
        tex->m_z      = BASE_Z + 1;
        tex->m_width  = drawW;
        tex->m_height = drawH;
        renderQueueAdd(fid, tex);
    }

    // Placed points as small circles (scale frame coords -> screen)
    for(uint32_t i = 0; i < placed; i++)
    {
        float px, py;
        if(!getWirePoint(m_calibrateCam, i, px, py)) continue;

        float sx = drawX + (px / frameW) * drawW;
        float sy = drawY + (py / frameH) * drawH;

        // Triple ring = yellow, double ring = cyan
        Color color = (i < WIRE_POINTS_PER_RING) ? Color{255, 220, 80} : Color{80, 220, 255};
        drawDot(sx, sy, POINT_RADIUS, color, BASE_Z + 3);
    }
}


// ============================================================================
// Target guide
// ============================================================================
//
// "Triple 5/20" said how far through the sequence you were but not where to
// click, so this panel spells the target out three ways: the ring and the two
// beds whose dividing wire it sits on, a diagram with that exact corner
// marked, and a reminder that it is always the ring's OUTER edge - the
// intersection furthest from the bull, never the inner one.

void CalibrationScreen::renderTargetGuide(uint32_t placed)
{
    FrameID fid = getFrameId();

    // Panel background
    {
        auto bg = std::make_shared<RenderShape>();
        bg->m_type   = ShapeType::Box;
        bg->m_color  = {32, 34, 42};
        bg->m_x      = GUIDE_X;
        bg->m_y      = CAL_VIEW_TOP;
        bg->m_z      = BASE_Z + 1;
        bg->m_width  = GUIDE_W;
        bg->m_height = CAL_VIEW_H;
        renderQueueAdd(fid, bg);
    }

    const float centreX  = GUIDE_X + GUIDE_W * 0.5f;
    const float diagramY = CAL_VIEW_TOP + 78.0f + DIAGRAM_RADIUS * NUMBER_RATIO;

    WireTarget target;
    if(!getNextWireTarget(m_calibrateCam, target))
    {
        drawText("ALL 40 POINTS PLACED", m_guideFontId, {120, 220, 120},
                 centreX, CAL_VIEW_TOP + GUIDE_PAD, BASE_Z + 2, true);
        renderBoardDiagram(centreX, diagramY, DIAGRAM_RADIUS, placed, target);
        drawText("Esc to go back, then S to save.", m_smallFontId, {170, 175, 185},
                 centreX, diagramY + DIAGRAM_RADIUS * NUMBER_RATIO + 40.0f,
                 BASE_Z + 2, true);
        return;
    }

    const Color ringColor = target.tripleRing ? Color{255, 220, 80} : Color{80, 220, 255};

    drawText("CLICK THIS INTERSECTION", m_guideFontId, {200, 205, 215},
             centreX, CAL_VIEW_TOP + GUIDE_PAD, BASE_Z + 2, true);

    renderBoardDiagram(centreX, diagramY, DIAGRAM_RADIUS, placed, target);

    float textY = diagramY + DIAGRAM_RADIUS * NUMBER_RATIO + 30.0f;

    // Which ring, and which of its two edges
    drawText(std::string("OUTER edge of the ") +
             (target.tripleRing ? "TRIPLE" : "DOUBLE") + " ring",
             m_guideFontId, ringColor, centreX, textY, BASE_Z + 2, true);
    textY += GUIDE_PT + 10.0f;

    // Which wire, named by the two beds it separates
    drawText("on the wire between " + std::to_string(target.sectionBefore) +
             " and " + std::to_string(target.sectionAfter),
             m_guideFontId, {235, 235, 245}, centreX, textY, BASE_Z + 2, true);
    textY += GUIDE_PT + 18.0f;

    // Say "outer" once more - it is the easy thing to get wrong
    const char* hintA = target.tripleRing
        ? "The outermost corner of the triple:"
        : "The outermost corner of the double:";
    const char* hintB = target.tripleRing
        ? "the side facing the double ring."
        : "the outer rim of the scoring area.";
    drawText(hintA, m_smallFontId, {170, 175, 185}, centreX, textY, BASE_Z + 2, true);
    textY += SMALL_PT + 6.0f;
    drawText(hintB, m_smallFontId, {170, 175, 185}, centreX, textY, BASE_Z + 2, true);
    textY += SMALL_PT + 16.0f;

    drawText("Left click place    Right click undo", m_smallFontId, {140, 145, 155},
             centreX, textY, BASE_Z + 2, true);
}


void CalibrationScreen::renderBoardDiagram(float cx, float cy, float radius,
                                           uint32_t placed, const WireTarget& target)
{
    const Color faceColor = {24, 26, 32};

    // Board face
    drawDot(cx, cy, radius * 1.04f, faceColor, BASE_Z + 2);

    // Both rings as annuli: a filled disc with the face colour punched back
    // out of the middle. Drawn much wider than their true proportions so the
    // inner and outer edge of each ring are clearly two different places.
    const float doubleOuter = radius;
    const float doubleInner = radius * 0.90f;
    const float tripleOuter = radius * WIRE_TRIPLE_RADIUS_RATIO;
    const float tripleInner = tripleOuter * 0.88f;

    drawDot(cx, cy, doubleOuter,    {60, 130, 155}, BASE_Z + 3);
    drawDot(cx, cy, doubleInner,    faceColor,      BASE_Z + 4);
    drawDot(cx, cy, tripleOuter,    {160, 135, 55}, BASE_Z + 5);
    drawDot(cx, cy, tripleInner,    faceColor,      BASE_Z + 6);
    drawDot(cx, cy, radius * 0.06f, {70, 75, 85},   BASE_Z + 7);

    const float deg2rad   = static_cast<float>(M_PI) / 180.0f;
    const bool  hasTarget = (placed < WIRE_POINTS_PER_CAMERA);

    // The 20 wires, bull to rim
    for(uint32_t i = 0; i < WIRE_POINTS_PER_RING; i++)
    {
        float angle = (WIRE_START_ANGLE_DEG +
                       static_cast<float>(i) * WIRE_SEGMENT_ANGLE_DEG) * deg2rad;

        bool  isTarget  = hasTarget && (i == target.wireIndex);
        Color wireColor = isTarget ? Color{255, 255, 255} : Color{95, 100, 112};
        float thickness = isTarget ? 4.0f : 2.0f;

        // A box of length `radius` rotated so its local +x runs along `angle`,
        // inner end at the bull. m_x/m_y is the top-left of the unrotated box.
        float midX = cx + std::cos(angle) * radius * 0.5f;
        float midY = cy + std::sin(angle) * radius * 0.5f;

        auto wire = std::make_shared<RenderShape>();
        wire->m_type     = ShapeType::Box;
        wire->m_color    = wireColor;
        wire->m_x        = midX - radius * 0.5f;
        wire->m_y        = midY - thickness * 0.5f;
        wire->m_z        = BASE_Z + (isTarget ? 9 : 8);
        wire->m_width    = radius;
        wire->m_height   = thickness;
        wire->m_rotation = angle;
        renderQueueAdd(getFrameId(), wire);
    }

    // Points already placed, so progress around the board is visible
    for(uint32_t i = 0; i < placed; i++)
    {
        WireTarget done;
        if(!getWireTarget(i, done)) continue;

        float angle = done.angleDeg * deg2rad;
        float r     = done.tripleRing ? tripleOuter : doubleOuter;
        drawDot(cx + std::cos(angle) * r, cy + std::sin(angle) * r,
                3.5f, {110, 200, 110}, BASE_Z + 10);
    }

    // Bed numbers. Bed `sectionBefore` of wire i is centred half a segment
    // counter-clockwise of that wire.
    for(uint32_t i = 0; i < WIRE_POINTS_PER_RING; i++)
    {
        WireTarget bed;
        if(!getWireTarget(i, bed)) continue;

        float angle = (bed.angleDeg - WIRE_SEGMENT_ANGLE_DEG * 0.5f) * deg2rad;
        bool  adjacent = hasTarget &&
                         ((i == target.wireIndex) ||
                          (i == (target.wireIndex + 1) % WIRE_POINTS_PER_RING));

        drawText(std::to_string(bed.sectionBefore), m_smallFontId,
                 adjacent ? Color{255, 255, 255} : Color{120, 125, 135},
                 cx + std::cos(angle) * radius * NUMBER_RATIO,
                 cy + std::sin(angle) * radius * NUMBER_RATIO - SMALL_PT * 0.6f,
                 BASE_Z + 11, true);
    }

    if(!hasTarget)
    {
        return;
    }

    // The target: a hollow marker straddling the exact corner, so the
    // intersection underneath it stays visible.
    const Color ringColor = target.tripleRing ? Color{255, 220, 80} : Color{80, 220, 255};
    float angle = target.angleDeg * deg2rad;
    float r     = target.tripleRing ? tripleOuter : doubleOuter;
    float mx    = cx + std::cos(angle) * r;
    float my    = cy + std::sin(angle) * r;

    drawDot(mx, my, MARKER_RADIUS,        ringColor, BASE_Z + 12);
    drawDot(mx, my, MARKER_RADIUS - 3.0f, faceColor, BASE_Z + 13);
    drawDot(mx, my, 2.0f,                 ringColor, BASE_Z + 14);
}


// ============================================================================
// Data collection (debug only)
// ============================================================================

#ifndef NDEBUG
void CalibrationScreen::saveFrames()
{
    if(m_cameraCount == 0)
    {
        showStatus("No cameras to save from");
        return;
    }

    Status stat = saveAllCameraFrames(appDataPath("captures"));
    showStatus(IS_STATUS_OK(stat) ? "Frames saved to ./captures/" : "Failed to save frames");
}
#endif
