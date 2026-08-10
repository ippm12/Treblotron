/**
 * connection_settings.cpp
 *
 * The connection settings overlay and the always-on link indicator.
 *
 * These are GameManager members but live in their own file: game_manager.cpp is
 * already long, and this is a self-contained feature — everything here concerns
 * the link to a remote inference server and nothing else.
 *
 * Settings is an overlay rather than a screen of its own because it has to be
 * reachable mid-game. loadGame() tears down whatever is running, and the whole
 * point of being able to open this during a leg is to fix a dropped connection
 * without abandoning the leg.
 */

#include <SDL3/SDL.h>
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_keycode.h>
#include <SDL3_ttf/SDL_ttf.h>

#include <string>

#include "common_inc.hpp"
#include "frame/frame.hpp"
#include "frame/render_queue.hpp"
#include "game_lib/components/render_shape.hpp"
#include "game_lib/components/render_text.hpp"
#include "game_manager_class.hpp"
#include "vision/vision_link.hpp"


namespace
{
    /** Above the pause overlay's own layers so the keyboard is never buried. */
    constexpr uint32_t SETTINGS_OVERLAY_Z = 500;

    /** Above normal game content, below the settings overlay. */
    constexpr uint32_t LINK_INDICATOR_Z = 480;

    /** Rows in the settings overlay, in display order. */
    enum SettingsRow : uint8_t
    {
        SETTINGS_ROW_ADDRESS = 0,
        SETTINGS_ROW_CLOSE,
        SETTINGS_ROW_COUNT
    };

    /** Longest address we accept: "255.255.255.255:65535" plus room for a name. */
    constexpr size_t SETTINGS_ADDRESS_MAX = 64;

    Color linkColor(VisionLinkState state)
    {
        switch(state)
        {
            case VisionLinkState::Healthy:       return {70, 200, 100};
            case VisionLinkState::Degraded:      return {225, 185, 60};
            case VisionLinkState::Disconnected:  return {225, 80, 70};
            case VisionLinkState::NotApplicable: break;
        }
        return {120, 120, 130};
    }

    const char* linkLabel(VisionLinkState state)
    {
        switch(state)
        {
            case VisionLinkState::Healthy:       return "Server connected";
            case VisionLinkState::Degraded:      return "Server slow";
            case VisionLinkState::Disconnected:  return "No inference server";
            case VisionLinkState::NotApplicable: break;
        }
        return "";
    }
}


void GameManager::openSettings()
{
    m_settingsOpen   = true;
    m_settingsCursor = SETTINGS_ROW_ADDRESS;
    m_settingsStatus.clear();
    m_settingsKeyboard.close();
}


void GameManager::handleSettingsKey(uint32_t keycode)
{
    // The address editor swallows input while it is up.
    if(m_settingsKeyboard.isOpen())
    {
        const VirtualKeyboardResult result = m_settingsKeyboard.handleKey(keycode);
        if(result == VirtualKeyboardResult::Confirmed)
        {
            if(IS_STATUS_OK(setInferenceServerAddress(m_settingsKeyboard.getText())))
            {
                // No restart needed: the client re-reads the address on its next
                // reconnect, which is a couple of seconds away at most.
                m_settingsStatus = "Saved - reconnecting";
            }
            else
            {
                m_settingsStatus = "Could not save the address";
            }
            m_settingsKeyboard.close();
            SDL_StopTextInput(SDL_GetKeyboardFocus());
        }
        else if(result == VirtualKeyboardResult::Cancelled)
        {
            m_settingsKeyboard.close();
            SDL_StopTextInput(SDL_GetKeyboardFocus());
        }
        return;
    }

    switch(keycode)
    {
        case SDLK_UP:
            m_settingsCursor = static_cast<uint8_t>(
                (m_settingsCursor + SETTINGS_ROW_COUNT - 1) % SETTINGS_ROW_COUNT);
            break;

        case SDLK_DOWN:
            m_settingsCursor = static_cast<uint8_t>(
                (m_settingsCursor + 1) % SETTINGS_ROW_COUNT);
            break;

        case SDLK_RETURN:
        case SDLK_KP_ENTER:
            if(m_settingsCursor == SETTINGS_ROW_ADDRESS)
            {
                m_settingsStatus.clear();
                m_settingsKeyboard.open(getInferenceServerAddress(), SETTINGS_ADDRESS_MAX);
                SDL_StartTextInput(SDL_GetKeyboardFocus());
            }
            else
            {
                m_settingsOpen = false;
            }
            break;

        case SDLK_ESCAPE:
            m_settingsOpen = false;
            break;

        default:
            break;
    }
}


void GameManager::renderSettings()
{
    auto add = [&](std::shared_ptr<RenderObject> obj) { renderQueueAdd(m_frameId, obj); };

    auto box = [&](float x, float y, float w, float h, Color color, uint32_t z)
    {
        auto b = std::make_shared<RenderShape>();
        b->m_type   = ShapeType::Box;
        b->m_color  = color;
        b->m_x      = x;
        b->m_y      = y;
        b->m_z      = z;
        b->m_width  = w;
        b->m_height = h;
        add(b);
    };

    const FontID rowFontId = (m_pauseFontId != INVALID_FONT_ID) ? m_pauseFontId : m_barFontId;
    TTF_Font* rowFont = getFont(rowFontId);
    TTF_Font* barFont = getFont(m_barFontId);

    auto text = [&](const std::string& str, float x, float y, Color color, FontID font)
    {
        auto t = std::make_shared<RenderText>();
        t->m_text     = str;
        t->m_color    = color;
        t->m_fontId   = font;
        t->m_rotation = 0.0f;
        t->m_scaleX   = 1.0f;
        t->m_scaleY   = 1.0f;
        t->m_x        = x;
        t->m_y        = y;
        t->m_z        = SETTINGS_OVERLAY_Z + 3;
        add(t);
    };

    box(0.0f, 0.0f, 1920.0f, 1080.0f, {20, 20, 20}, SETTINGS_OVERLAY_Z);

    constexpr float panelW = 980.0f;
    constexpr float panelH = 470.0f;
    const float panelX = (1920.0f - panelW) * 0.5f;
    const float panelY = (1080.0f - panelH) * 0.5f;
    box(panelX, panelY, panelW, panelH, {50, 50, 55}, SETTINGS_OVERLAY_Z + 1);

    const char* title = "Connection";
    int titleW = 0, titleH = 0;
    if(barFont) TTF_GetStringSize(barFont, title, 0, &titleW, &titleH);
    text(title, panelX + (panelW - titleW) * 0.5f, panelY + 26.0f, {200, 200, 200}, m_barFontId);

    // Live status, so the effect of an edit shows up without leaving the page.
    const VisionLinkState state = getVisionLinkState();

    auto dot = std::make_shared<RenderShape>();
    dot->m_type  = ShapeType::Circle;
    dot->m_color = linkColor(state);
    dot->m_x     = panelX + 62.0f;
    dot->m_y     = panelY + 122.0f;
    dot->m_width = 26.0f;
    dot->m_z     = SETTINGS_OVERLAY_Z + 3;
    add(dot);

    text(linkLabel(state),       panelX + 96.0f, panelY + 104.0f, linkColor(state), rowFontId);
    text(getVisionLinkDetail(),  panelX + 96.0f, panelY + 150.0f, {160, 160, 170},  rowFontId);

    const std::string address = getInferenceServerAddress();
    const std::string rows[SETTINGS_ROW_COUNT] = {
        "Server address:  " + (address.empty() ? std::string("(not set)") : address),
        "Close"
    };

    constexpr float rowH   = 72.0f;
    const float     rowW   = panelW - 96.0f;
    const float     rowX   = panelX + 48.0f;
    const float     startY = panelY + 214.0f;

    for(uint8_t i = 0; i < SETTINGS_ROW_COUNT; i++)
    {
        const float rowY     = startY + i * rowH;
        const bool  selected = (i == m_settingsCursor);

        if(selected)
        {
            box(rowX, rowY, rowW, rowH - 10.0f, {70, 70, 120}, SETTINGS_OVERLAY_Z + 2);
        }

        int rw = 0, rh = 0;
        if(rowFont) TTF_GetStringSize(rowFont, rows[i].c_str(), 0, &rw, &rh);
        text(rows[i], rowX + 20.0f, rowY + (rowH - 10.0f - rh) * 0.5f,
             selected ? Color{255, 255, 255} : Color{170, 170, 180}, rowFontId);
    }

    if(!m_settingsStatus.empty())
    {
        int sw = 0, sh = 0;
        if(rowFont) TTF_GetStringSize(rowFont, m_settingsStatus.c_str(), 0, &sw, &sh);
        text(m_settingsStatus, panelX + (panelW - sw) * 0.5f, panelY + panelH - 72.0f,
             {150, 210, 160}, rowFontId);
    }

    // The keyboard covers the panel while typing, so the hints would just be
    // noise underneath it.
    if(m_settingsKeyboard.isOpen())
    {
        m_settingsKeyboard.render(m_frameId, SETTINGS_OVERLAY_Z + 10);
    }
    else
    {
        m_inputHints.render(m_frameId, rowFontId, panelX + 48.0f, panelY + panelH - 36.0f,
                            SETTINGS_OVERLAY_Z + 3,
                            {{SDLK_RETURN, SDL_GAMEPAD_BUTTON_SOUTH, "select"},
                             {SDLK_ESCAPE, SDL_GAMEPAD_BUTTON_EAST,  "close"}});
    }
}


void GameManager::renderLinkIndicator()
{
    const VisionLinkState state = getVisionLinkState();

    // Builds that do their own inference have no link to report on.
    if(state == VisionLinkState::NotApplicable) return;

    // The settings overlay already shows a larger version of the same thing.
    if(m_settingsOpen) return;

    // Always-on dot in the corner: green, amber or red at a glance.
    auto dot = std::make_shared<RenderShape>();
    dot->m_type  = ShapeType::Circle;
    dot->m_color = linkColor(state);
    dot->m_x     = 1880.0f;
    dot->m_y     = 34.0f;
    dot->m_width = 18.0f;
    dot->m_z     = LINK_INDICATOR_Z;
    renderQueueAdd(m_frameId, dot);

    if(state != VisionLinkState::Disconnected) return;

    // Down: say so in words and say what to press. Scoring has silently stopped
    // working, and without this the only symptom is darts not registering.
    const FontID fontId = (m_pauseFontId != INVALID_FONT_ID) ? m_pauseFontId : m_barFontId;
    TTF_Font* font = getFont(fontId);
    const std::string message = std::string(linkLabel(state)) + " - press F1 for settings";

    int tw = 0, th = 0;
    if(font) TTF_GetStringSize(font, message.c_str(), 0, &tw, &th);

    const float bannerW = static_cast<float>(tw) + 56.0f;
    const float bannerH = static_cast<float>(th) + 22.0f;
    const float bannerX = (1920.0f - bannerW) * 0.5f;

    auto banner = std::make_shared<RenderShape>();
    banner->m_type   = ShapeType::Box;
    banner->m_color  = {70, 26, 24};
    banner->m_x      = bannerX;
    banner->m_y      = 18.0f;
    banner->m_z      = LINK_INDICATOR_Z;
    banner->m_width  = bannerW;
    banner->m_height = bannerH;
    renderQueueAdd(m_frameId, banner);

    auto label = std::make_shared<RenderText>();
    label->m_text     = message;
    label->m_color    = {245, 175, 170};
    label->m_fontId   = fontId;
    label->m_rotation = 0.0f;
    label->m_scaleX   = 1.0f;
    label->m_scaleY   = 1.0f;
    label->m_x        = bannerX + 28.0f;
    label->m_y        = 29.0f;
    label->m_z        = LINK_INDICATOR_Z + 1;
    renderQueueAdd(m_frameId, label);
}
