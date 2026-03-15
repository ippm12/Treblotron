/**
 * render_shape.hpp
 *
 * RenderShape is a renderable object and Flecs component that
 * draws a basic geometric shape (box, circle, or arc).
 */

#ifndef RENDER_SHAPE_HPP
#define RENDER_SHAPE_HPP

#include <memory>
#include "game_lib/components/render_object.hpp"

enum class ShapeType
{
    Box,
    Circle,
    Arc
};

class RenderShape;

typedef std::shared_ptr<RenderShape> RenderShapePtr;

class RenderShape : public RenderObject
{
    public:
        Status render(FrameID frameId) const override;

        ShapeType m_type;
        Color     m_color;
        float     m_width;      // Box: width.  Circle: diameter.  Arc: ending radius.
        float     m_height;     // Box: height. Circle: ignored.   Arc: starting radius.
        float     m_startAngle; // Arc only (degrees)
        float     m_endAngle;   // Arc only (degrees)
};

#endif // RENDER_SHAPE_HPP
