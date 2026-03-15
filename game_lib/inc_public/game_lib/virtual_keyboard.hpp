/**
 * virtual_keyboard.hpp
 *
 * On-screen virtual keyboard for controller/gamepad text input.
 * Renders a QWERTY keyboard overlay that can be navigated with
 * D-pad and accepts input via A/B/X/Y buttons.
 */

#ifndef VIRTUAL_KEYBOARD_HPP
#define VIRTUAL_KEYBOARD_HPP

#include "common_inc.hpp"
#include "frame/frame.hpp"
#include "game_lib/input_hints.hpp"
#include <string>
#include <vector>

enum class VirtualKeyboardResult
{
    Active,     // Still open, user is typing
    Confirmed,  // User pressed confirm - read getText()
    Cancelled   // User pressed cancel
};

class VirtualKeyboard
{
public:
    void init(FontID keyFont, FontID textFont);

    /**
     * Open the keyboard with an initial text buffer.
     * The keyboard becomes active and will intercept input.
     */
    void open(const std::string& initialText, size_t maxLen);

    /** Close the keyboard. */
    void close();

    /** Returns true if the keyboard is currently open. */
    bool isOpen() const;

    /**
     * Feed a keycode to the keyboard (from key or gamepad handler).
     * Returns the current result state.
     */
    VirtualKeyboardResult handleKey(uint32_t keycode);

    /**
     * Feed raw text input from a physical keyboard.
     * Appends to the buffer so physical keyboard works alongside.
     */
    VirtualKeyboardResult handleTextInput(const char* text);

    /** Get the current text buffer. */
    const std::string& getText() const;

    /**
     * Render the keyboard overlay.
     * Call each frame when isOpen() == true.
     */
    void render(FrameID frameId, uint32_t zBase);

private:
    struct KeyDef
    {
        std::string label;
        std::string value;
        float       width;   // multiplier relative to standard key (1.0)
        enum Action { Char, Backspace, Confirm, Shift, Clear } action;
    };

    std::vector<std::vector<KeyDef>> m_rows;

    // State
    bool        m_open    = false;
    bool        m_shifted = false;
    std::string m_buffer;
    size_t      m_maxLen  = 20;
    int         m_cursorRow = 0;
    int         m_cursorCol = 0;

    // Fonts
    FontID m_keyFont  = INVALID_FONT_ID;
    FontID m_textFont = INVALID_FONT_ID;

    // Input hints
    InputHints m_inputHints;

    // Shortcut icons for action keys (gamepad + keyboard variants)
    ImageID m_iconShiftGamepad   = INVALID_IMAGE_ID;
    ImageID m_iconShiftKeyboard  = INVALID_IMAGE_ID;
    ImageID m_iconBackGamepad    = INVALID_IMAGE_ID;
    ImageID m_iconBackKeyboard   = INVALID_IMAGE_ID;
    ImageID m_iconClearGamepad   = INVALID_IMAGE_ID;
    ImageID m_iconClearKeyboard  = INVALID_IMAGE_ID;

    // Layout helpers
    void buildLayout();
    void clampCursor();
    int  closestColInRow(int fromRow, int fromCol, int toRow) const;
    float keyCenterX(int row, int col) const;
    VirtualKeyboardResult pressCurrentKey();
};

#endif // VIRTUAL_KEYBOARD_HPP
