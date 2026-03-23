/**
 * input_hints.cpp
 *
 * Loads keyboard and Xbox controller icons and renders hint bars.
 */

#include "game_lib/input_hints.hpp"
#include "game_lib/components/render_image.hpp"
#include "game_lib/components/render_text.hpp"
#include "frame/render_queue.hpp"

#include <SDL3/SDL_keycode.h>
#include <SDL3/SDL_gamepad.h>
#include <SDL3_ttf/SDL_ttf.h>


static const char* KB_BASE = "assets/images/kenney_input-prompts_1.4.1/Keyboard & Mouse/Default/";
static const char* XB_BASE = "assets/images/kenney_input-prompts_1.4.1/Xbox Series/Default/";

static constexpr float ICON_SIZE  = 36.0f;
static constexpr float ICON_GAP   = 8.0f;   // gap between icon and label
static constexpr float HINT_GAP   = 31.0f;  // gap between hint groups


static ImageID loadIcon(const char* basePath, const char* filename)
{
    std::string path = std::string(basePath) + filename;
    return loadImage(path.c_str());
}


void InputHints::init()
{
    // Keyboard icons
    m_keyboardIcons[SDLK_UP]        = loadIcon(KB_BASE, "keyboard_arrow_up.png");
    m_keyboardIcons[SDLK_DOWN]      = loadIcon(KB_BASE, "keyboard_arrow_down.png");
    m_keyboardIcons[SDLK_LEFT]      = loadIcon(KB_BASE, "keyboard_arrow_left.png");
    m_keyboardIcons[SDLK_RIGHT]     = loadIcon(KB_BASE, "keyboard_arrow_right.png");
    m_keyboardIcons[SDLK_RETURN]    = loadIcon(KB_BASE, "keyboard_enter.png");
    m_keyboardIcons[SDLK_ESCAPE]    = loadIcon(KB_BASE, "keyboard_escape.png");
    m_keyboardIcons[SDLK_BACKSPACE] = loadIcon(KB_BASE, "keyboard_backspace.png");
    m_keyboardIcons[SDLK_DELETE]    = loadIcon(KB_BASE, "keyboard_delete.png");
    m_keyboardIcons[SDLK_TAB]       = loadIcon(KB_BASE, "keyboard_tab.png");
    m_keyboardIcons[SDLK_HOME]      = loadIcon(KB_BASE, "keyboard_home.png");
    m_keyboardIcons[SDLK_S]         = loadIcon(KB_BASE, "keyboard_s.png");

    // Xbox colored button icons
    m_gamepadIcons[SDL_GAMEPAD_BUTTON_SOUTH]      = loadIcon(XB_BASE, "xbox_button_color_a.png");
    m_gamepadIcons[SDL_GAMEPAD_BUTTON_EAST]       = loadIcon(XB_BASE, "xbox_button_color_b.png");
    m_gamepadIcons[SDL_GAMEPAD_BUTTON_WEST]       = loadIcon(XB_BASE, "xbox_button_color_x.png");
    m_gamepadIcons[SDL_GAMEPAD_BUTTON_NORTH]      = loadIcon(XB_BASE, "xbox_button_color_y.png");
    m_gamepadIcons[SDL_GAMEPAD_BUTTON_DPAD_UP]    = loadIcon(XB_BASE, "xbox_dpad_up.png");
    m_gamepadIcons[SDL_GAMEPAD_BUTTON_DPAD_DOWN]  = loadIcon(XB_BASE, "xbox_dpad_down.png");
    m_gamepadIcons[SDL_GAMEPAD_BUTTON_DPAD_LEFT]  = loadIcon(XB_BASE, "xbox_dpad_left.png");
    m_gamepadIcons[SDL_GAMEPAD_BUTTON_DPAD_RIGHT] = loadIcon(XB_BASE, "xbox_dpad_right.png");
    m_gamepadIcons[SDL_GAMEPAD_BUTTON_START]      = loadIcon(XB_BASE, "xbox_button_start.png");
    m_gamepadIcons[SDL_GAMEPAD_BUTTON_BACK]       = loadIcon(XB_BASE, "xbox_button_back.png");

    // Shoulder buttons
    m_gamepadIcons[SDL_GAMEPAD_BUTTON_LEFT_SHOULDER]  = loadIcon(XB_BASE, "xbox_lb.png");

    // Virtual icon: left stick
    m_gamepadIcons[GAMEPAD_ICON_LEFT_STICK]       = loadIcon(XB_BASE, "xbox_stick_l.png");
}


void InputHints::shutdown()
{
    for(auto& [key, id] : m_keyboardIcons)
    {
        if(id != INVALID_IMAGE_ID)
        {
            unloadImage(id);
        }
    }
    m_keyboardIcons.clear();

    for(auto& [btn, id] : m_gamepadIcons)
    {
        if(id != INVALID_IMAGE_ID)
        {
            unloadImage(id);
        }
    }
    m_gamepadIcons.clear();
}


void InputHints::render(FrameID frameId, FontID fontId, float x, float y,
                        uint32_t z, const std::vector<InputHint>& hints)
{
    bool useGamepad = (getLastInputDevice() == InputDevice::Gamepad);
    float curX = x;

    for(const auto& hint : hints)
    {
        // Look up the icon
        ImageID iconId = INVALID_IMAGE_ID;
        if(useGamepad)
        {
            auto it = m_gamepadIcons.find(hint.gamepadButton);
            if(it != m_gamepadIcons.end())
            {
                iconId = it->second;
            }
        }
        else
        {
            auto it = m_keyboardIcons.find(hint.keyboardKey);
            if(it != m_keyboardIcons.end())
            {
                iconId = it->second;
            }
        }

        // Render icon
        if(iconId != INVALID_IMAGE_ID)
        {
            auto img = std::make_shared<RenderImage>();
            img->m_imageId = iconId;
            img->m_width   = ICON_SIZE;
            img->m_height  = ICON_SIZE;
            img->m_x       = curX;
            img->m_y       = y;
            img->m_z       = z + 1;
            renderQueueAdd(frameId, img);
            curX += ICON_SIZE + ICON_GAP;
        }

        // Render label text
        if(!hint.label.empty())
        {
            auto text = std::make_shared<RenderText>();
            text->m_text     = hint.label;
            text->m_color    = {160, 160, 170};
            text->m_fontId   = fontId;
            text->m_rotation = 0.0f;
            text->m_scaleX   = 1.0f;
            text->m_scaleY   = 1.0f;
            text->m_x        = curX;
            text->m_y        = y + 2.0f;  // slight vertical offset to align with icon
            text->m_z        = z + 1;
            renderQueueAdd(frameId, text);

            // Estimate text width (approximate)
            TTF_Font* font = getFont(fontId);
            if(font)
            {
                int textW = 0, textH = 0;
                TTF_GetStringSize(font, hint.label.c_str(), 0, &textW, &textH);
                curX += static_cast<float>(textW);
            }
            else
            {
                curX += hint.label.length() * 10.0f;
            }
        }

        curX += HINT_GAP;
    }
}
