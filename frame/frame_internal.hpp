/**
 * frame_internal.hpp
 *
 * Internal header shared between frame module source files.
 * Not part of the public API.
 */

#ifndef FRAME_INTERNAL_HPP
#define FRAME_INTERNAL_HPP

#include <SDL3/SDL.h>
#include "frame/frame.hpp"

/**
 * Get the SDL renderer for a given frame. Returns nullptr if the frame ID is invalid.
 */
SDL_Renderer* getFrameRenderer(FrameID id);

/**
 * Destroy all cached text textures for a given frame.
 * Called when a frame is deleted or the font system shuts down.
 */
void clearTextCache(FrameID frameId);

/**
 * Remove all cached text textures that use a specific font,
 * across all frames. Called when a font is unloaded.
 */
void evictTextCacheByFont(FontID fontId);

#endif // FRAME_INTERNAL_HPP
