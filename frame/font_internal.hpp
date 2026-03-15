/**
 * font_internal.hpp
 *
 * Internal header for the font subsystem. Provides initialization
 * and shutdown functions called by the frame module lifecycle.
 * Not part of the public API.
 */

#ifndef FONT_INTERNAL_HPP
#define FONT_INTERNAL_HPP

#include "common_inc.hpp"

/** 
 * Initialize SDL_ttf and the internal font storage. Called by initializeFrameModule().
 */
Status initializeFontSystem();

/**
 * Shut down SDL_ttf and release all loaded fonts. Called by shutdownFrameModule(). 
 */
void shutdownFontSystem();

#endif // FONT_INTERNAL_HPP
