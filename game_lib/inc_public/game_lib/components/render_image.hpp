/**
 * render_image.hpp
 *
 * A RenderObject that renders a loaded image by ImageID.
 * Resolves the texture lazily via getImageTexture().
 */

#ifndef RENDER_IMAGE_HPP
#define RENDER_IMAGE_HPP

#include "game_lib/components/render_object.hpp"
#include "frame/image.hpp"

class RenderImage : public RenderObject
{
    public:
        Status render(FrameID frameId) const override;

    public:
        ImageID m_imageId = INVALID_IMAGE_ID;
        float   m_width   = 0.0f;   // 0 = use natural size
        float   m_height  = 0.0f;   // 0 = use natural size
};

#endif // RENDER_IMAGE_HPP
