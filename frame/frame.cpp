/**
 * frame.cpp
 * 
 * Manages windows
 */

#include <mutex>
#include <SDL3/SDL.h>
#include "common_inc.hpp"
#include "frame/frame.hpp"
#include "frame/image.hpp"
#include "frame_internal.hpp"
#include "font_internal.hpp"


typedef struct
{
    bool active;
    SDL_Window* win;
    SDL_Renderer* rend;
    SDL_WindowID id;
    size_t width;
    size_t height;
} FrameInfo;


// A pointer to each frame
static FrameInfo f_frames[MAX_NUM_FRAMES];

// Per-frame click handlers
static FrameClickCallback f_clickHandlers[MAX_NUM_FRAMES];

// Per-frame key handlers
static FrameKeyCallback f_keyHandlers[MAX_NUM_FRAMES];

// Per-frame text input handlers
static FrameTextCallback f_textHandlers[MAX_NUM_FRAMES];

// Per-frame gamepad button handlers
static FrameGamepadButtonCallback f_gamepadButtonHandlers[MAX_NUM_FRAMES];

// Open gamepad handles
#define MAX_GAMEPADS 4
static SDL_Gamepad* f_gamepads[MAX_GAMEPADS] = {};

// Last input device used
static InputDevice f_lastInputDevice = InputDevice::Keyboard;

// Stick-to-digital conversion state
static constexpr int16_t STICK_DEADZONE = 16000;
static bool f_stickLeft  = false;
static bool f_stickRight = false;
static bool f_stickUp    = false;
static bool f_stickDown  = false;

// Whether the frame module ahs been initialized
static bool f_frameModuleInitialized = false;

Status initializeFrameModule()
{
    LOG_INFO(FRAME_LOG_ID, "Starting frame initialization");

    if(f_frameModuleInitialized)
    {
        LOG_ERROR(FRAME_LOG_ID, "Frame Module already initialized");
        return STATUS_ERROR_INVALID_STATE;
    }

    // Null out local windows and click handlers
    for(int i = 0; i < MAX_NUM_FRAMES; i++)
    {
        f_frames[i].active = false;
        f_frames[i].win = nullptr;
        f_frames[i].rend = nullptr;
        f_frames[i].id = 0;
        f_frames[i].width = 0;
        f_frames[i].height = 0;
        f_clickHandlers[i] = nullptr;
        f_keyHandlers[i] = nullptr;
        f_textHandlers[i] = nullptr;
        f_gamepadButtonHandlers[i] = nullptr;
    }

    for(int i = 0; i < MAX_GAMEPADS; i++)
    {
        f_gamepads[i] = nullptr;
    }

    if(!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD))
    {
        // SDL failed intiialization
        LOG_ERROR(FRAME_LOG_ID, "SDL failed to init video");
        return STATUS_ERROR_GENERIC;
    }

    Status fontStatus = initializeFontSystem();
    if(IS_STATUS_NOT_OK(fontStatus))
    {
        LOG_ERROR(FRAME_LOG_ID, "Failed to initialize font system");
        return fontStatus;
    }

    Status imgStatus = initializeImageSystem();
    if(IS_STATUS_NOT_OK(imgStatus))
    {
        LOG_ERROR(FRAME_LOG_ID, "Failed to initialize image system");
        return imgStatus;
    }

    f_frameModuleInitialized = true;

    LOG_INFO(FRAME_LOG_ID, "Frame module initialized");

    return STATUS_OK;
}


void shutdownFrameModule()
{
    // Shutdown all windows and renderers
    for(int i = 0; i < MAX_NUM_FRAMES; i++)
    {
        if(f_frames[i].active)
        {
            deleteFrame(i);
        }
    }

    // Close any open gamepads
    for(int i = 0; i < MAX_GAMEPADS; i++)
    {
        if(f_gamepads[i])
        {
            SDL_CloseGamepad(f_gamepads[i]);
            f_gamepads[i] = nullptr;
        }
    }

    // Shutdown image and font systems before SDL
    shutdownImageSystem();
    shutdownFontSystem();

    // Shutdown SDL library
    SDL_Quit();
}


Status createNewFrame(const std::string& name, size_t width, size_t height, FrameID& frameIdOutput)
{
    if(!f_frameModuleInitialized)
    {
        LOG_ERROR(FRAME_LOG_ID, "Must initialize frame module before creating frames");
        return STATUS_ERROR_NOT_INIT;
    }

    SDL_WindowFlags flags = 0;
    if((0 == width) && (0 == height))
    {
        flags = SDL_WINDOW_MAXIMIZED;
    }

    SDL_Window* win = nullptr;
    SDL_Renderer* rend = nullptr;

    if(!SDL_CreateWindowAndRenderer(name.c_str(), width, height, flags, &win, &rend))
    {
        // SDL failed to create window or renderer
        LOG_ERROR(FRAME_LOG_ID, "SDL Failed to create window or renderer");
        return STATUS_ERROR_GENERIC;
    }

    if((nullptr == win) || (nullptr == rend))
    {
        // SDL returned ok but window or renderer is NULL for some reason
        LOG_ERROR(FRAME_LOG_ID, "SDL window or renderer returned NULL");
        return STATUS_ERROR_NULL;
    }

    int actualWidth, actualHeight;
    if(!SDL_GetWindowSizeInPixels(win, &actualWidth, &actualHeight))
    {
        // SDL failed to get the window size
        LOG_ERROR(FRAME_LOG_ID, "SDL Failed to get window size");
        SDL_DestroyRenderer(rend);
        SDL_DestroyWindow(win);
        return STATUS_ERROR_GENERIC;
    }

    if( ((0 != width) && ((size_t)actualWidth != width)) || 
        ((0 != height) && ((size_t)actualHeight != height)) )
    {
        // Actual width does not match requested
        LOG_WARNING(FRAME_LOG_ID, "SDL returned window of size {} x {} when {} x {} was requested", 
                                    actualWidth, actualHeight, width, height);
    }

    SDL_WindowID winID = SDL_GetWindowID(win);
    if(0 == winID)
    {
        LOG_ERROR(FRAME_LOG_ID, "Unable to get window ID");
        SDL_DestroyRenderer(rend);
        SDL_DestroyWindow(win);
        return STATUS_ERROR_GENERIC;
    }

    FrameID frameID = INVALID_FRAME_ID;

    for(int i = 0; i < MAX_NUM_FRAMES; i++)
    {
        if(!f_frames[i].active)
        {
            frameID = i;
            f_frames[i].active = true;
            f_frames[i].win = win;
            f_frames[i].rend = rend;
            f_frames[i].id = winID;
            f_frames[i].width = actualWidth;
            f_frames[i].height = actualHeight;
            break;
        }
    }

    Status stat = STATUS_OK;
    if(INVALID_FRAME_ID == frameID)
    {
        LOG_ERROR(FRAME_LOG_ID, "Only {} windows can be open at once", MAX_NUM_FRAMES);
        stat = STATUS_ERROR_GENERIC;
        SDL_DestroyRenderer(rend);
        SDL_DestroyWindow(win);
    }

    // Return the frame ID to the user
    frameIdOutput = frameID;

    return stat;
}


void deleteFrame(FrameID frameId)
{
    if(frameId > MAX_NUM_FRAMES)
    {
        LOG_ERROR(FRAME_LOG_ID, "Tried to remove invalid frame ID: {}", frameId);
        return;
    }

    // Clear text cache before destroying the renderer
    clearTextCache(frameId);

    SDL_Window* win = nullptr;
    SDL_Renderer* rend = nullptr;

    // Remove the frame from active
    if(f_frames[frameId].active)
    {
        f_frames[frameId].active = false;
        win = f_frames[frameId].win;
        f_frames[frameId].win = nullptr;
        rend = f_frames[frameId].rend;
        f_frames[frameId].rend = nullptr;
        f_frames[frameId].id = 0;
        f_clickHandlers[frameId] = nullptr;
        f_keyHandlers[frameId] = nullptr;
        f_textHandlers[frameId] = nullptr;
        f_gamepadButtonHandlers[frameId] = nullptr;
    }

    // Destroy the frame
    if(rend)
    {
        SDL_DestroyRenderer(rend);
    }
    if(win)
    {
        SDL_DestroyWindow(win);
    }

    return;
}


Status setFrameFullscreen(FrameID id, bool isFull)
{
    if(!f_frameModuleInitialized)
    {
        LOG_ERROR(FRAME_LOG_ID, "Must initialize frame module before setting frame to fullscreen");
        return STATUS_ERROR_NOT_INIT;
    }

    if(id > MAX_NUM_FRAMES)
    {
        LOG_ERROR(FRAME_LOG_ID, "Tried to set fullscreen for invalid frame ID: {}", id);
        return STATUS_ERROR_INVALID_PARAM;
    }

    if(!f_frames[id].active)
    {
        LOG_ERROR(FRAME_LOG_ID, "Could not set fullscreen for inactive frame");
        return STATUS_ERROR_INVALID_STATE;
    }

    if(!SDL_SetWindowFullscreen(f_frames[id].win, isFull))
    {
        LOG_ERROR(FRAME_LOG_ID, "SDL failed to set window fullscreen to {}", isFull ? "true": "false");
        return STATUS_ERROR_GENERIC;
    }

    // Now that we have resized the window, get its new dimensions
    int tempWidth = 0;
    int tempHeight = 0;
    if(!SDL_GetWindowSizeInPixels(f_frames[id].win, &tempWidth, &tempHeight))
    {
        LOG_ERROR(FRAME_LOG_ID, "SDL failed to get window size");
        return STATUS_ERROR_GENERIC;
    }

    f_frames[id].width = tempWidth;
    f_frames[id].height = tempHeight;

    return STATUS_OK;
}


Status getFrameSize(FrameID id, size_t& width, size_t& height)
{
    if(!f_frameModuleInitialized)
    {
        LOG_ERROR(FRAME_LOG_ID, "Must initialize frame module before getting frame size");
        return STATUS_ERROR_NOT_INIT;
    }

    if(id > MAX_NUM_FRAMES)
    {
        LOG_ERROR(FRAME_LOG_ID, "Tried to set fullscreen for invalid frame ID: {}", id);
        return STATUS_ERROR_INVALID_PARAM;
    }

    if(!f_frames[id].active)
    {
        LOG_ERROR(FRAME_LOG_ID, "Could not set fullscreen for inactive frame");
        return STATUS_ERROR_INVALID_STATE;
    }

    width = f_frames[id].width;
    height = f_frames[id].height;

    return STATUS_OK;
}


Status presentFrame(FrameID id)
{
    if(id >= MAX_NUM_FRAMES || !f_frames[id].active)
    {
        LOG_ERROR(FRAME_LOG_ID, "Invalid frame ID for present: {}", id);
        return STATUS_ERROR_INVALID_PARAM;
    }

    if(!SDL_RenderPresent(f_frames[id].rend))
    {
        LOG_ERROR(FRAME_LOG_ID, "SDL failed to present frame: {}", SDL_GetError());
        return STATUS_ERROR_LIB_CALL;
    }

    return STATUS_OK;
}


static FrameID convertSDLWindowIdToInternal(SDL_WindowID sdlId)
{
    for(FrameID i = 0; i < MAX_NUM_FRAMES; i++)
    {
        if(f_frames[i].active && (f_frames[i].id == sdlId))
        {
            // Found matching ID
            return i;
        }
    }

    // Not found, return invalid ID
    return INVALID_FRAME_ID;
}


bool pollFrames()
{
    SDL_Event evt;
    bool continueRunning = true;

    while(SDL_PollEvent(&evt))
    {
        switch(evt.type)
        {
            case SDL_EVENT_QUIT:
                continueRunning = false;
                break;
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            {
                FrameID closedId = convertSDLWindowIdToInternal(evt.window.windowID);
                if(closedId == 0)
                {
                    // Main frame closed — signal program exit
                    continueRunning = false;
                }
                else
                {
                    deleteFrame(closedId);
                }
                break;
            }
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
            {
                FrameID fid = convertSDLWindowIdToInternal(evt.button.windowID);
                if(fid != INVALID_FRAME_ID && f_clickHandlers[fid])
                {
                    f_clickHandlers[fid](fid, evt.button.x, evt.button.y, evt.button.button);
                }
                break;
            }
            case SDL_EVENT_KEY_DOWN:
            case SDL_EVENT_KEY_UP:
            {
                if(evt.type == SDL_EVENT_KEY_DOWN)
                {
                    f_lastInputDevice = InputDevice::Keyboard;
                }
                FrameID fid = convertSDLWindowIdToInternal(evt.key.windowID);
                if(fid != INVALID_FRAME_ID && f_keyHandlers[fid])
                {
                    f_keyHandlers[fid](fid, evt.key.key, evt.type == SDL_EVENT_KEY_DOWN);
                }
                break;
            }
            case SDL_EVENT_TEXT_INPUT:
            {
                FrameID fid = convertSDLWindowIdToInternal(evt.text.windowID);
                if(fid != INVALID_FRAME_ID && f_textHandlers[fid])
                {
                    f_textHandlers[fid](fid, evt.text.text);
                }
                break;
            }
            case SDL_EVENT_GAMEPAD_ADDED:
            {
                SDL_JoystickID jid = evt.gdevice.which;
                if(SDL_IsGamepad(jid))
                {
                    // Find an empty slot
                    for(int i = 0; i < MAX_GAMEPADS; i++)
                    {
                        if(f_gamepads[i] == nullptr)
                        {
                            f_gamepads[i] = SDL_OpenGamepad(jid);
                            if(f_gamepads[i])
                            {
                                LOG_INFO(FRAME_LOG_ID, "Gamepad connected: {}",
                                    SDL_GetGamepadName(f_gamepads[i]));
                            }
                            break;
                        }
                    }
                }
                break;
            }
            case SDL_EVENT_GAMEPAD_REMOVED:
            {
                SDL_JoystickID jid = evt.gdevice.which;
                for(int i = 0; i < MAX_GAMEPADS; i++)
                {
                    if(f_gamepads[i] && SDL_GetGamepadID(f_gamepads[i]) == jid)
                    {
                        LOG_INFO(FRAME_LOG_ID, "Gamepad disconnected");
                        SDL_CloseGamepad(f_gamepads[i]);
                        f_gamepads[i] = nullptr;
                        break;
                    }
                }
                break;
            }
            case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
            case SDL_EVENT_GAMEPAD_BUTTON_UP:
            {
                if(evt.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN)
                {
                    f_lastInputDevice = InputDevice::Gamepad;
                }
                // Gamepad events are not window-specific; dispatch to frame 0
                FrameID fid = 0;
                if(f_frames[fid].active && f_gamepadButtonHandlers[fid])
                {
                    f_gamepadButtonHandlers[fid](fid, evt.gbutton.button,
                        evt.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN);
                }
                break;
            }
            case SDL_EVENT_GAMEPAD_AXIS_MOTION:
            {
                f_lastInputDevice = InputDevice::Gamepad;
                FrameID fid = 0;
                if(!f_frames[fid].active || !f_gamepadButtonHandlers[fid])
                {
                    break;
                }

                uint8_t axis  = evt.gaxis.axis;
                int16_t value = evt.gaxis.value;

                if(axis == SDL_GAMEPAD_AXIS_LEFTX)
                {
                    bool nowLeft  = (value < -STICK_DEADZONE);
                    bool nowRight = (value >  STICK_DEADZONE);
                    if(nowLeft && !f_stickLeft)
                        f_gamepadButtonHandlers[fid](fid, SDL_GAMEPAD_BUTTON_DPAD_LEFT, true);
                    if(!nowLeft && f_stickLeft)
                        f_gamepadButtonHandlers[fid](fid, SDL_GAMEPAD_BUTTON_DPAD_LEFT, false);
                    if(nowRight && !f_stickRight)
                        f_gamepadButtonHandlers[fid](fid, SDL_GAMEPAD_BUTTON_DPAD_RIGHT, true);
                    if(!nowRight && f_stickRight)
                        f_gamepadButtonHandlers[fid](fid, SDL_GAMEPAD_BUTTON_DPAD_RIGHT, false);
                    f_stickLeft  = nowLeft;
                    f_stickRight = nowRight;
                }
                else if(axis == SDL_GAMEPAD_AXIS_LEFTY)
                {
                    bool nowUp   = (value < -STICK_DEADZONE);
                    bool nowDown = (value >  STICK_DEADZONE);
                    if(nowUp && !f_stickUp)
                        f_gamepadButtonHandlers[fid](fid, SDL_GAMEPAD_BUTTON_DPAD_UP, true);
                    if(!nowUp && f_stickUp)
                        f_gamepadButtonHandlers[fid](fid, SDL_GAMEPAD_BUTTON_DPAD_UP, false);
                    if(nowDown && !f_stickDown)
                        f_gamepadButtonHandlers[fid](fid, SDL_GAMEPAD_BUTTON_DPAD_DOWN, true);
                    if(!nowDown && f_stickDown)
                        f_gamepadButtonHandlers[fid](fid, SDL_GAMEPAD_BUTTON_DPAD_DOWN, false);
                    f_stickDown = nowDown;
                    f_stickUp   = nowUp;
                }
                break;
            }
        }
    }

    return continueRunning;
}


SDL_Renderer* getFrameRenderer(FrameID id)
{
    if(id >= MAX_NUM_FRAMES || !f_frames[id].active)
    {
        return nullptr;
    }
    return f_frames[id].rend;
}


Status registerFrameClickHandler(FrameID frameId, FrameClickCallback callback)
{
    if(frameId >= MAX_NUM_FRAMES || !f_frames[frameId].active)
    {
        LOG_ERROR(FRAME_LOG_ID, "Cannot register click handler for invalid frame: {}", frameId);
        return STATUS_ERROR_INVALID_PARAM;
    }

    f_clickHandlers[frameId] = callback;
    return STATUS_OK;
}


void unregisterFrameClickHandler(FrameID frameId)
{
    if(frameId < MAX_NUM_FRAMES)
    {
        f_clickHandlers[frameId] = nullptr;
    }
}


Status registerFrameKeyHandler(FrameID frameId, FrameKeyCallback callback)
{
    if(frameId >= MAX_NUM_FRAMES || !f_frames[frameId].active)
    {
        LOG_ERROR(FRAME_LOG_ID, "Cannot register key handler for invalid frame: {}", frameId);
        return STATUS_ERROR_INVALID_PARAM;
    }

    f_keyHandlers[frameId] = callback;
    return STATUS_OK;
}


void unregisterFrameKeyHandler(FrameID frameId)
{
    if(frameId < MAX_NUM_FRAMES)
    {
        f_keyHandlers[frameId] = nullptr;
    }
}


Status registerFrameTextHandler(FrameID frameId, FrameTextCallback callback)
{
    if(frameId >= MAX_NUM_FRAMES || !f_frames[frameId].active)
    {
        LOG_ERROR(FRAME_LOG_ID, "Cannot register text handler for invalid frame: {}", frameId);
        return STATUS_ERROR_INVALID_PARAM;
    }

    f_textHandlers[frameId] = callback;
    return STATUS_OK;
}


void unregisterFrameTextHandler(FrameID frameId)
{
    if(frameId < MAX_NUM_FRAMES)
    {
        f_textHandlers[frameId] = nullptr;
    }
}


Status registerFrameGamepadButtonHandler(FrameID frameId, FrameGamepadButtonCallback callback)
{
    if(frameId >= MAX_NUM_FRAMES || !f_frames[frameId].active)
    {
        LOG_ERROR(FRAME_LOG_ID, "Cannot register gamepad handler for invalid frame: {}", frameId);
        return STATUS_ERROR_INVALID_PARAM;
    }

    f_gamepadButtonHandlers[frameId] = callback;
    return STATUS_OK;
}


void unregisterFrameGamepadButtonHandler(FrameID frameId)
{
    if(frameId < MAX_NUM_FRAMES)
    {
        f_gamepadButtonHandlers[frameId] = nullptr;
    }
}


InputDevice getLastInputDevice()
{
    return f_lastInputDevice;
}
