/**
 * frame.cpp
 * 
 * Manages windows
 */

#include <mutex>
#include <SDL3/SDL.h>
#include "common_inc.hpp"
#include "frame/frame.hpp"

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

    // Null out local windows
    for(int i = 0; i < MAX_NUM_FRAMES; i++)
    {
        f_frames[i].active = false;
        f_frames[i].win = nullptr;
        f_frames[i].rend = nullptr;
        f_frames[i].id = 0;
        f_frames[i].width = 0;
        f_frames[i].height = 0;
    }

    if(!SDL_Init(SDL_INIT_VIDEO))
    {
        // SDL failed intiialization
        LOG_ERROR(FRAME_LOG_ID, "SDL failed to init video");
        return STATUS_ERROR_GENERIC;
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
        LOG_INFO(FRAME_LOG_ID, "Received event: {}", evt.type);
        switch(evt.type)
        {
            case SDL_EVENT_QUIT:
                continueRunning = false;
                break;
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                deleteFrame(convertSDLWindowIdToInternal(evt.window.windowID));
                break;
        }
    }

    return continueRunning;
}
