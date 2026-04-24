/**
 * calibration.cpp
 *
 * Camera calibration screen implementation.
 */

#include "calibration.hpp"
#include "vision/vision.hpp"
#include "vision/wire_calibration.hpp"
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
#include <memory>


// ============================================================================
// Layout constants
// ============================================================================

static constexpr float    WINDOW_W       = 1920.0f;
static constexpr float    WINDOW_H       = 1080.0f;

static constexpr float    TITLE_PT       = 72.0f;
static constexpr float    BODY_PT        = 39.0f;

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

// Calibrate: large centered view
static constexpr float    CAL_VIEW_TOP   = 120.0f;
static constexpr float    CAL_VIEW_H     = 760.0f;
static constexpr float    CAL_VIEW_MAXW  = 1400.0f;

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

    if(!m_cameraTextures[m_calibrateCam] ||
       m_lastFrameW[m_calibrateCam] <= 0.0f || m_lastFrameH[m_calibrateCam] <= 0.0f)
    {
        auto text = std::make_shared<RenderText>();
        text->m_text     = "No frame available";
        text->m_color    = {200, 200, 80};
        text->m_fontId   = m_bodyFontId;
        text->m_rotation = 0.0f;
        text->m_scaleX   = 1.0f;
        text->m_scaleY   = 1.0f;
        text->m_x        = LEFT_MARGIN;
        text->m_y        = CAL_VIEW_TOP + CAL_VIEW_H * 0.5f;
        text->m_z        = BASE_Z + 2;
        renderQueueAdd(fid, text);
        return;
    }

    // Compute large draw rect
    float frameW = m_lastFrameW[m_calibrateCam];
    float frameH = m_lastFrameH[m_calibrateCam];
    float scale  = std::min(CAL_VIEW_MAXW / frameW, CAL_VIEW_H / frameH);
    float drawW  = frameW * scale;
    float drawH  = frameH * scale;
    float drawX  = (WINDOW_W - drawW) * 0.5f;
    float drawY  = CAL_VIEW_TOP + (CAL_VIEW_H - drawH) * 0.5f;

    m_calibrateDrawRect = {drawX, drawY, drawW, drawH};

    // Camera image
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

    // Camera label + next-point counter
    std::string header = getCameraName(m_calibrateCam) + "   ";
    std::string label = getNextPointLabel(m_calibrateCam);
    if(label.empty())
    {
        header += "calibrated (40/40)";
    }
    else
    {
        header += "next: " + label + "   (" +
                  std::to_string(getWirePointCount(m_calibrateCam)) + "/40)";
    }

    auto headerText = std::make_shared<RenderText>();
    headerText->m_text     = header;
    headerText->m_color    = {220, 220, 230};
    headerText->m_fontId   = m_bodyFontId;
    headerText->m_rotation = 0.0f;
    headerText->m_scaleX   = 1.0f;
    headerText->m_scaleY   = 1.0f;
    headerText->m_x        = LEFT_MARGIN;
    headerText->m_y        = TITLE_Y + TITLE_PT + 8.0f;
    headerText->m_z        = BASE_Z + 2;
    renderQueueAdd(fid, headerText);

    // Placed points as small circles (scale frame coords → screen)
    uint32_t placed = getWirePointCount(m_calibrateCam);
    for(uint32_t i = 0; i < placed; i++)
    {
        float px, py;
        if(!getWirePoint(m_calibrateCam, i, px, py)) continue;

        float sx = drawX + (px / frameW) * drawW;
        float sy = drawY + (py / frameH) * drawH;

        // Triple ring = yellow, double ring = cyan
        Color color = (i < 20) ? Color{255, 220, 80} : Color{80, 220, 255};

        auto dot = std::make_shared<RenderShape>();
        dot->m_type   = ShapeType::Circle;
        dot->m_color  = color;
        dot->m_x      = sx;
        dot->m_y      = sy;
        dot->m_z      = BASE_Z + 3;
        dot->m_width  = POINT_RADIUS * 2.0f;
        dot->m_height = 0.0f;
        renderQueueAdd(fid, dot);
    }
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

    Status stat = saveAllCameraFrames("./captures");
    showStatus(IS_STATUS_OK(stat) ? "Frames saved to ./captures/" : "Failed to save frames");
}
#endif
