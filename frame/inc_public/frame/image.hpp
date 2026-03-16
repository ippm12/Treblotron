/**
 * image.hpp
 *
 * Image loading and texture management for PNG/JPG files.
 * Uses SDL_image for file loading and caches SDL_Texture per frame.
 */

#ifndef IMAGE_HPP
#define IMAGE_HPP

#include "frame/frame.hpp"

#define MAX_NUM_IMAGES      256
#define INVALID_IMAGE_ID    SIZE_MAX
typedef size_t ImageID;

/**
 * Initialize the image subsystem. Called by initializeFrameModule().
 */
Status initializeImageSystem();

/**
 * Shut down the image subsystem. Called by shutdownFrameModule().
 */
void shutdownImageSystem();

/**
 * Load an image from a file (PNG, JPG, BMP, etc.).
 * Returns an ImageID handle, or INVALID_IMAGE_ID on failure.
 */
ImageID loadImage(const char* filePath);

/**
 * Unload a previously loaded image and free its textures.
 */
void unloadImage(ImageID id);

/**
 * Get the SDL_Texture for an image on a specific frame's renderer.
 * Creates and caches the texture lazily on first call per frame.
 * Returns nullptr if the ImageID is invalid.
 */
struct SDL_Texture;
SDL_Texture* getImageTexture(FrameID frameId, ImageID id);

/**
 * Get the natural width and height of a loaded image in pixels.
 */
void getImageSize(ImageID id, float& width, float& height);

#endif // IMAGE_HPP
