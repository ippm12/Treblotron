/**
 * render_cached_texture.hpp
 *
 * A RenderObject that blits a pre-rendered SDL_Texture.
 * Does NOT own the texture — the creator manages its lifetime.
 */

#ifndef RENDER_CACHED_TEXTURE_HPP
#define RENDER_CACHED_TEXTURE_HPP

#include "game_lib/components/render_object.hpp"

struct SDL_Texture;

class RenderCachedTexture : public RenderObject
{
    public:
        Status render(FrameID frameId) const override;

    public:
        SDL_Texture* m_texture = nullptr;
        float        m_width   = 0.0f;
        float        m_height  = 0.0f;
};

#endif // RENDER_CACHED_TEXTURE_HPP
