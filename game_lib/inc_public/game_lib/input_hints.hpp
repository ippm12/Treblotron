/**
 * input_hints.hpp
 *
 * Renders icon+label input hint bars that auto-switch between
 * keyboard and controller icons based on the last input device.
 */

#ifndef INPUT_HINTS_HPP
#define INPUT_HINTS_HPP

#include "frame/frame.hpp"
#include "frame/image.hpp"
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

/** Virtual gamepad icon IDs for non-button icons. */
static constexpr uint8_t GAMEPAD_ICON_LEFT_STICK = 254;

/** A single input hint: one keyboard key, one gamepad button, and a label. */
struct InputHint
{
    uint32_t    keyboardKey;     // SDL_Keycode (e.g. SDLK_RETURN)
    uint8_t     gamepadButton;   // SDL_GamepadButton or GAMEPAD_ICON_* constant
    std::string label;           // e.g. "select", "back"
};

class InputHints
{
    public:
        /** Load all icon images from the Kenney asset pack. */
        void init();

        /** Unload all icon images. */
        void shutdown();

        /**
         * Render a horizontal row of [icon] label hints.
         * Automatically chooses keyboard or gamepad icons based on last input.
         */
        void render(FrameID frameId, FontID fontId, float x, float y,
                    uint32_t z, const std::vector<InputHint>& hints);

    private:
        std::unordered_map<uint32_t, ImageID> m_keyboardIcons;
        std::unordered_map<uint8_t, ImageID>  m_gamepadIcons;
};

#endif // INPUT_HINTS_HPP
