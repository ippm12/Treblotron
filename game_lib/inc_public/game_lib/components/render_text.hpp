/**
 * render_text.hpp
 *
 * RenderText is a renderable object and Flecs component that
 * draws text using a loaded TrueType font.
 */

#ifndef RENDER_TEXT_HPP
#define RENDER_TEXT_HPP

#include <memory>
#include <string>
#include "game_lib/components/render_object.hpp"

class RenderText;

typedef std::shared_ptr<RenderText> RenderTextPtr;

class RenderText : public RenderObject
{
    public:
        Status render(FrameID frameId) const override;

        std::string m_text;
        Color       m_color;
        FontID      m_fontId;
        float       m_rotation; // Degrees, clockwise (0 = no rotation)
        float       m_scaleX;   // Horizontal scale (1.0 = natural size)
        float       m_scaleY;   // Vertical scale (1.0 = natural size)
};

#endif // RENDER_TEXT_HPP
