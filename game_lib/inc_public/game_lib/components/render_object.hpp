/**
 * render_object.hpp
 *
 * Base class for all renderable objects. Subclass this to create
 * new renderable types (e.g., RenderShape, RenderImage).
 */

#ifndef RENDER_OBJECT_HPP
#define RENDER_OBJECT_HPP

#include <memory>
#include "common_inc.hpp"
#include "frame/frame.hpp"

/** RGB color with 8-bit channels. */
struct Color
{
    uint8_t r; /** Red channel (0–255) */
    uint8_t g; /** Green channel (0–255) */
    uint8_t b; /** Blue channel (0–255) */
};

class RenderObject;

typedef std::shared_ptr<RenderObject> RenderObjectPtr;

/**
 * Base class for all renderable objects. Subclass this to create
 * new renderable types (e.g., RenderShape, RenderImage).
 */
class RenderObject
{
    public:
        virtual ~RenderObject() = default;

        /** Draw this object to the given frame. */
        virtual Status render(FrameID frameId) const = 0;

    public:
        float       m_x; // horizontal position in pixels
        float       m_y; // vertical position in pixels
        uint32_t    m_z; // Draw order, higher values render on top
};

#endif // RENDER_OBJECT_HPP
