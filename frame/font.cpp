/**
 * font.cpp
 *
 * Font loading and management using SDL_ttf.
 * TTF_Init/TTF_Quit are called from initializeFrameModule/shutdownFrameModule.
 */

#include <SDL3_ttf/SDL_ttf.h>
#include "common_inc.hpp"
#include "frame/frame.hpp"
#include "font_internal.hpp"
#include "frame_internal.hpp"


static TTF_Font* f_fonts[MAX_NUM_FONTS] = {};
static bool f_ttfInitialized = false;


Status initializeFontSystem()
{
    if(f_ttfInitialized)
    {
        return STATUS_OK;
    }

    if(!TTF_Init())
    {
        LOG_ERROR(FRAME_LOG_ID, "SDL_ttf failed to initialize: {}", SDL_GetError());
        return STATUS_ERROR_LIB_CALL;
    }

    for(int i = 0; i < MAX_NUM_FONTS; i++)
    {
        f_fonts[i] = nullptr;
    }

    f_ttfInitialized = true;
    LOG_INFO(FRAME_LOG_ID, "Font system initialized");

    return STATUS_OK;
}


void shutdownFontSystem()
{
    if(!f_ttfInitialized)
    {
        return;
    }

    for(int i = 0; i < MAX_NUM_FONTS; i++)
    {
        if(f_fonts[i])
        {
            TTF_CloseFont(f_fonts[i]);
            f_fonts[i] = nullptr;
        }
    }

    TTF_Quit();
    f_ttfInitialized = false;
}


FontID loadFont(const char* filePath, float ptSize)
{
    if(!f_ttfInitialized)
    {
        LOG_ERROR(FRAME_LOG_ID, "Font system not initialized");
        return INVALID_FONT_ID;
    }

    if(!filePath)
    {
        LOG_ERROR(FRAME_LOG_ID, "Null font file path");
        return INVALID_FONT_ID;
    }

    // Find an empty slot
    FontID slot = INVALID_FONT_ID;
    for(int i = 0; i < MAX_NUM_FONTS; i++)
    {
        if(!f_fonts[i])
        {
            slot = i;
            break;
        }
    }

    if(slot == INVALID_FONT_ID)
    {
        LOG_ERROR(FRAME_LOG_ID, "No free font slots (max {})", MAX_NUM_FONTS);
        return INVALID_FONT_ID;
    }

    TTF_Font* font = TTF_OpenFont(filePath, ptSize);
    if(!font)
    {
        LOG_ERROR(FRAME_LOG_ID, "Failed to load font '{}': {}", filePath, SDL_GetError());
        return INVALID_FONT_ID;
    }

    f_fonts[slot] = font;
    LOG_INFO(FRAME_LOG_ID, "Loaded font '{}' at {}pt (ID: {})", filePath, ptSize, slot);

    return slot;
}


void unloadFont(FontID fontId)
{
    if(fontId >= MAX_NUM_FONTS)
    {
        LOG_ERROR(FRAME_LOG_ID, "Invalid font ID: {}", fontId);
        return;
    }

    if(f_fonts[fontId])
    {
        evictTextCacheByFont(fontId);
        TTF_CloseFont(f_fonts[fontId]);
        f_fonts[fontId] = nullptr;
    }
}


TTF_Font* getFont(FontID id)
{
    if(id >= MAX_NUM_FONTS)
    {
        return nullptr;
    }
    return f_fonts[id];
}
