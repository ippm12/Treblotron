/**
 * calibration.cpp
 *
 * Camera calibration and data collection screen implementation.
 */

#include "calibration.hpp"
#include "vision/vision.hpp"
#include "game_lib/game_manager.hpp"
#include "games/main_menu.hpp"
#include "game_lib/components/render_shape.hpp"
#include "game_lib/components/render_text.hpp"
#include "frame/render_queue.hpp"

#include <SDL3/SDL_keycode.h>
#include <SDL3/SDL_gamepad.h>
#include <SDL3_ttf/SDL_ttf.h>

#include <memory>


// ============================================================================
// Layout constants
// ============================================================================

static constexpr float    WINDOW_W       = 1280.0f;
static constexpr float    WINDOW_H       = 720.0f;

static constexpr float    TITLE_PT       = 48.0f;
static constexpr float    BODY_PT        = 26.0f;

static constexpr float    LEFT_MARGIN    = 40.0f;
static constexpr float    TITLE_Y        = 15.0f;
static constexpr uint32_t BASE_Z         = 10;

// Camera slot layout (3 columns)
static constexpr float    SLOT_TOP       = 80.0f;
static constexpr float    SLOT_GAP       = 20.0f;
static constexpr float    SLOT_W         = (WINDOW_W - LEFT_MARGIN * 2.0f - SLOT_GAP * 2.0f) / 3.0f;
static constexpr float    SLOT_H         = 420.0f;

// Status message
static constexpr float    STATUS_Y       = 520.0f;
static constexpr float    STATUS_FADE    = 3.0f;

// Input hints
static constexpr float    HINTS_Y        = 575.0f;


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

    Status stat = initializeCameraSystem();
    if(IS_STATUS_NOT_OK(stat))
    {
        return stat;
    }

    m_cameraCount = getCameraCount();
    return STATUS_OK;
}


void CalibrationScreen::update(float deltaTime)
{
    if(m_statusTimer > 0.0f)
    {
        m_statusTimer -= deltaTime;
    }
}


void CalibrationScreen::render()
{
    FrameID fid = getFrameId();

    // Title
    {
        auto text = std::make_shared<RenderText>();
        text->m_text     = "Calibration";
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

    renderCameraSlots();
    renderStatusMessage();

    // Input hints
    std::vector<InputHint> hints;
    hints.push_back({SDLK_ESCAPE, SDL_GAMEPAD_BUTTON_EAST, "back"});
#ifndef NDEBUG
    hints.push_back({SDLK_S, SDL_GAMEPAD_BUTTON_NORTH, "save frames"});
#endif
    m_inputHints.render(fid, m_bodyFontId, LEFT_MARGIN, HINTS_Y, BASE_Z, hints);
}


void CalibrationScreen::shutdown()
{
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
    switch(keycode)
    {
        case SDLK_ESCAPE:
            loadGame(std::make_shared<MainMenu>());
            break;
#ifndef NDEBUG
        case SDLK_S:
            saveFrames();
            break;
#endif
        default:
            break;
    }
}


void CalibrationScreen::onGamepadButton(uint8_t button, bool pressed)
{
    if(!pressed) return;

    switch(button)
    {
        case SDL_GAMEPAD_BUTTON_EAST:  // B button
            onKeyDown(SDLK_ESCAPE);
            break;
#ifndef NDEBUG
        case SDL_GAMEPAD_BUTTON_NORTH: // Y button
            onKeyDown(SDLK_S);
            break;
#endif
        default:
            break;
    }
}


// ============================================================================
// Rendering
// ============================================================================

void CalibrationScreen::renderCameraSlots()
{
    FrameID fid = getFrameId();

    for(uint32_t i = 0; i < EXPECTED_CAMERA_COUNT; i++)
    {
        float slotX = LEFT_MARGIN + static_cast<float>(i) * (SLOT_W + SLOT_GAP);
        float slotY = SLOT_TOP;

        // Slot background
        auto bg = std::make_shared<RenderShape>();
        bg->m_type   = ShapeType::Box;
        bg->m_color  = {40, 40, 48};
        bg->m_x      = slotX;
        bg->m_y      = slotY;
        bg->m_z      = BASE_Z;
        bg->m_width  = SLOT_W;
        bg->m_height = SLOT_H;
        renderQueueAdd(fid, bg);

        // Camera name label
        std::string name = getCameraName(i);
        auto nameText = std::make_shared<RenderText>();
        nameText->m_text     = name;
        nameText->m_color    = {180, 180, 190};
        nameText->m_fontId   = m_bodyFontId;
        nameText->m_rotation = 0.0f;
        nameText->m_scaleX   = 1.0f;
        nameText->m_scaleY   = 1.0f;
        nameText->m_x        = slotX + 15.0f;
        nameText->m_y        = slotY + 10.0f;
        nameText->m_z        = BASE_Z + 1;
        renderQueueAdd(fid, nameText);

        // Status text (centered in slot)
        std::string statusStr;
        Color statusColor;

        if(i < m_cameraCount)
        {
            SDL_Surface* frame = getCameraFrame(i);
            if(frame)
            {
                // Future: render camera frame as texture
                statusStr = "Feed active";
                statusColor = {80, 200, 80};
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
            statusColor = {200, 80, 80};
        }

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
        statusText->m_z        = BASE_Z + 1;
        renderQueueAdd(fid, statusText);
    }
}


void CalibrationScreen::renderStatusMessage()
{
    if(m_statusTimer <= 0.0f || m_statusMessage.empty()) return;

    FrameID fid = getFrameId();

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
    text->m_z        = BASE_Z + 2;
    renderQueueAdd(fid, text);
}


// ============================================================================
// Data collection (debug only)
// ============================================================================

#ifndef NDEBUG
void CalibrationScreen::saveFrames()
{
    if(m_cameraCount == 0)
    {
        m_statusMessage = "No cameras to save from";
        m_statusTimer = STATUS_FADE;
        return;
    }

    for(uint32_t i = 0; i < EXPECTED_CAMERA_COUNT; i++)
    {
        if(i < m_cameraCount)
        {
            saveCameraFrame(i, "./captures");
        }
    }

    m_statusMessage = "Frames saved to ./captures/";
    m_statusTimer = STATUS_FADE;
}
#endif
