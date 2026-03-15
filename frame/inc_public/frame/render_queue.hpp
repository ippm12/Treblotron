/**
 * render_queue.hpp
 *
 * Public API for the render queue. Each frame has its own queue.
 * Collects RenderObject commands and draws them to the frame rendering system.
 */

#ifndef RENDER_QUEUE_HPP
#define RENDER_QUEUE_HPP

#include "game_lib/components/render_object.hpp"
#include "frame/frame.hpp"

/**
 * Enqueue a renderable object to the given frame.
 * The queue shares ownership with the caller.
 */
Status renderQueueAdd(FrameID frameId, RenderObjectPtr obj);

/**
 * Render all queued objects for the given frame, then clear its queue.
 */
Status renderQueueDrawFlush(FrameID frameId);

/**
 * Discard all queued objects for the given frame without rendering.
 */
Status renderQueueDiscard(FrameID frameId);

/**
 * Clear the frame with a background color before rendering.
 */
Status renderQueueClearFrame(FrameID frameId, uint8_t r, uint8_t g, uint8_t b);

#endif // RENDER_QUEUE_HPP
