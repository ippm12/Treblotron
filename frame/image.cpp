/**
 * image.cpp
 *
 * Image loading via SDL_image with per-frame texture caching.
 */

#include <SDL3/SDL.h>
#include <SDL3_image/SDL_image.h>
#include "common_inc.hpp"
#include "frame/image.hpp"
#include "frame/frame.hpp"


struct ImageEntry
{
    bool          active;
    SDL_Surface*  surface;
    SDL_Texture*  textures[MAX_NUM_FRAMES];  // Cached per frame
    float         width;
    float         height;
};

static ImageEntry f_images[MAX_NUM_IMAGES];
static bool f_imageSystemInitialized = false;


Status initializeImageSystem()
{
    for(size_t i = 0; i < MAX_NUM_IMAGES; i++)
    {
        f_images[i].active = false;
        f_images[i].surface = nullptr;
        f_images[i].width = 0.0f;
        f_images[i].height = 0.0f;
        for(size_t j = 0; j < MAX_NUM_FRAMES; j++)
        {
            f_images[i].textures[j] = nullptr;
        }
    }

    f_imageSystemInitialized = true;
    return STATUS_OK;
}


void shutdownImageSystem()
{
    if(!f_imageSystemInitialized) return;

    for(size_t i = 0; i < MAX_NUM_IMAGES; i++)
    {
        if(f_images[i].active)
        {
            unloadImage(i);
        }
    }

    f_imageSystemInitialized = false;
}


ImageID loadImage(const char* filePath)
{
    if(!f_imageSystemInitialized)
    {
        LOG_ERROR(FRAME_LOG_ID, "Image system not initialized");
        return INVALID_IMAGE_ID;
    }

    SDL_Surface* surface = IMG_Load(filePath);
    if(!surface)
    {
        LOG_ERROR(FRAME_LOG_ID, "Failed to load image '{}': {}", filePath, SDL_GetError());
        return INVALID_IMAGE_ID;
    }

    // Find empty slot
    for(size_t i = 0; i < MAX_NUM_IMAGES; i++)
    {
        if(!f_images[i].active)
        {
            f_images[i].active  = true;
            f_images[i].surface = surface;
            f_images[i].width   = static_cast<float>(surface->w);
            f_images[i].height  = static_cast<float>(surface->h);
            for(size_t j = 0; j < MAX_NUM_FRAMES; j++)
            {
                f_images[i].textures[j] = nullptr;
            }
            return i;
        }
    }

    LOG_ERROR(FRAME_LOG_ID, "No free image slots (max {})", MAX_NUM_IMAGES);
    SDL_DestroySurface(surface);
    return INVALID_IMAGE_ID;
}


void unloadImage(ImageID id)
{
    if(id >= MAX_NUM_IMAGES || !f_images[id].active) return;

    // Destroy cached textures
    for(size_t j = 0; j < MAX_NUM_FRAMES; j++)
    {
        if(f_images[id].textures[j])
        {
            SDL_DestroyTexture(f_images[id].textures[j]);
            f_images[id].textures[j] = nullptr;
        }
    }

    if(f_images[id].surface)
    {
        SDL_DestroySurface(f_images[id].surface);
        f_images[id].surface = nullptr;
    }

    f_images[id].active = false;
    f_images[id].width  = 0.0f;
    f_images[id].height = 0.0f;
}


SDL_Texture* getImageTexture(FrameID frameId, ImageID id)
{
    if(id >= MAX_NUM_IMAGES || !f_images[id].active) return nullptr;
    if(frameId >= MAX_NUM_FRAMES) return nullptr;

    // Return cached texture if available
    if(f_images[id].textures[frameId])
    {
        return f_images[id].textures[frameId];
    }

    // Create texture from surface
    SDL_Renderer* rend = getFrameRenderer(frameId);
    if(!rend || !f_images[id].surface) return nullptr;

    SDL_Texture* tex = SDL_CreateTextureFromSurface(rend, f_images[id].surface);
    if(!tex)
    {
        LOG_ERROR(FRAME_LOG_ID, "Failed to create texture from image: {}", SDL_GetError());
        return nullptr;
    }

    f_images[id].textures[frameId] = tex;
    return tex;
}


void getImageSize(ImageID id, float& width, float& height)
{
    if(id >= MAX_NUM_IMAGES || !f_images[id].active)
    {
        width = 0.0f;
        height = 0.0f;
        return;
    }

    width  = f_images[id].width;
    height = f_images[id].height;
}
