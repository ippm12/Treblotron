/**
 * frame.hpp
 * 
 * Functions for opening and initializing windows.
 * These fuynctions must only be accessed by the main thread.
 */

#ifndef FRAME_HPP
#define FRAME_HPP

#include <string>
#include "common_inc.hpp"

#define MAX_NUM_FRAMES      5

#define INVALID_FRAME_ID    SIZE_MAX
typedef size_t FrameID;

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
 * Poll all frames for events.
 * Return false if program should quit.
 */
bool pollFrames(); 

#endif // FRAME_HPP