/**
 * frame.hpp
 * 
 * Functions for opening and initializing windows.
 * These fuynctions must only be accessed by the main thread.
 */

#ifndef FRAME_HPP
#define FRAME_HPP

#include <string>
#include <functional>
#include "common_inc.hpp"

#define MAX_NUM_FRAMES      5

#define INVALID_FRAME_ID    SIZE_MAX
typedef size_t FrameID;

#define MAX_NUM_FONTS       16
#define INVALID_FONT_ID     SIZE_MAX
typedef size_t FontID;

/**
 * Initializes SDL and the frame module.  Does not create any windows.
 */
Status initializeFrameModule();

/**
 * Shuts down SDl and the frame module
 */
void shutdownFrameModule();

/**
 * Creates a new frame with given name, width, and height.  If width and height are 0,
 * then it will be the size of the main monitor.  Returns the frame ID in frameIdOutput.
 * The thread that calls this function must be the same thread that polls events.
 */
Status createNewFrame(const std::string& name, size_t width, size_t height, FrameID& frameIdOutput);

/**
 * Delets frame with the given ID.
 */
void deleteFrame(FrameID frameId);

/**
 * Set whetehr a given frame is fullscreen.
 */
Status setFrameFullscreen(FrameID id, bool isFull);

/**
 * Gets a frame's width and height in pixels
 */
Status getFrameSize(FrameID id, size_t& width, size_t& height);

/**
 * Present the rendered content for the given frame (swaps buffers).
 * Call this after renderQueueDrawFlush() to display the frame.
 */
Status presentFrame(FrameID id);

/**
 * Get the underlying SDL renderer for a frame.
 * Returns nullptr if the frame ID is invalid or the frame is not active.
 */
struct SDL_Renderer;
SDL_Renderer* getFrameRenderer(FrameID id);

/**
 * Poll all frames for events.
 * Return false if program should quit.
 */
bool pollFrames();

/**
 * Mouse click callback: receives (frameId, mouseX, mouseY, button).
 * button values: SDL_BUTTON_LEFT=1, SDL_BUTTON_MIDDLE=2, SDL_BUTTON_RIGHT=3.
 */
typedef std::function<void(FrameID, float, float, uint8_t)> FrameClickCallback;

/**
 * Register a click handler for a specific frame. Only one handler per frame.
 * Replaces any previously registered handler for the same frame.
 */
Status registerFrameClickHandler(FrameID frameId, FrameClickCallback callback);

/**
 * Remove the click handler for a specific frame.
 */
void unregisterFrameClickHandler(FrameID frameId);

/**
 * Key event callback: receives (frameId, keycode, isPressed).
 * keycode is an SDL_Keycode value (uint32_t). isPressed is true for
 * key-down, false for key-up.
 */
typedef std::function<void(FrameID, uint32_t, bool)> FrameKeyCallback;

/**
 * Register a key handler for a specific frame. Only one handler per frame.
 * Replaces any previously registered handler for the same frame.
 */
Status registerFrameKeyHandler(FrameID frameId, FrameKeyCallback callback);

/**
 * Remove the key handler for a specific frame.
 */
void unregisterFrameKeyHandler(FrameID frameId);

/**
 * Text input callback: receives (frameId, text).
 * text is a UTF-8 string from SDL_EVENT_TEXT_INPUT.
 * Call SDL_StartTextInput / SDL_StopTextInput to enable/disable.
 */
typedef std::function<void(FrameID, const char*)> FrameTextCallback;

/**
 * Register a text input handler for a specific frame. Only one handler per frame.
 * Replaces any previously registered handler for the same frame.
 */
Status registerFrameTextHandler(FrameID frameId, FrameTextCallback callback);

/**
 * Remove the text input handler for a specific frame.
 */
void unregisterFrameTextHandler(FrameID frameId);

/**
 * Gamepad button callback: receives (frameId, button, isPressed).
 * button is an SDL_GamepadButton value. Dispatched to frame 0 (main window).
 */
typedef std::function<void(FrameID, uint8_t, bool)> FrameGamepadButtonCallback;

/**
 * Register a gamepad button handler for a specific frame. Only one handler per frame.
 */
Status registerFrameGamepadButtonHandler(FrameID frameId, FrameGamepadButtonCallback callback);

/**
 * Remove the gamepad button handler for a specific frame.
 */
void unregisterFrameGamepadButtonHandler(FrameID frameId);

/**
 * Tracks which input device was used most recently.
 */
enum class InputDevice { Keyboard, Gamepad };
InputDevice getLastInputDevice();

/**
 * Direct-poll APIs for gamepad state. These query SDL's current state of
 * the (first) connected gamepad rather than relying on edge-triggered
 * events, which is what callers need for continuous-action input
 * (analogue stick movement, holding buttons during a frame, etc).
 *
 * `axis` is an SDL_GamepadAxis (e.g. SDL_GAMEPAD_AXIS_LEFTX). Returns a
 * value in [-1.0, 1.0]. Returns 0 if no gamepad is connected.
 */
float getGamepadAxis(uint8_t axis);

/**
 * `button` is an SDL_GamepadButton (e.g. SDL_GAMEPAD_BUTTON_DPAD_LEFT).
 * Returns true if the button is currently held on any connected gamepad.
 */
bool isGamepadButtonHeld(uint8_t button);

/**
 * Load a TrueType font from a file at a given point size.
 * Returns the FontID handle, or INVALID_FONT_ID on failure.
 */
FontID loadFont(const char* filePath, float ptSize);

/**
 * Unload a previously loaded font.
 */
void unloadFont(FontID fontId);

/**
 * Get the underlying TTF_Font pointer for a loaded font.
 * Returns nullptr if the FontID is invalid or the font is not loaded.
 */
struct TTF_Font;
TTF_Font* getFont(FontID id);

#endif // FRAME_HPP