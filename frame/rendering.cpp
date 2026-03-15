/**
 * rendering.cpp
 *
 * Rendering functions and render queue implementation.
 */

#include <cmath>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>
#include "common_inc.hpp"
#include "frame/frame.hpp"
#include "frame/render_queue.hpp"
#include "game_lib/components/render_shape.hpp"
#include "game_lib/components/render_text.hpp"
#include "game_lib/components/render_cached_texture.hpp"
#include "game_lib/components/render_image.hpp"
#include "frame_internal.hpp"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static constexpr size_t TEXT_CACHE_MAX = 128;

static std::vector<RenderObjectPtr> f_renderQueues[MAX_NUM_FRAMES];

// ============================================================================
// Text texture cache
// ============================================================================

struct TextCacheKey
{
    std::string text;
    FontID fontId;
    uint8_t r, g, b;

    bool operator==(const TextCacheKey& o) const
    {
        return text == o.text && fontId == o.fontId
            && r == o.r && g == o.g && b == o.b;
    }
};

struct TextCacheKeyHash
{
    size_t operator()(const TextCacheKey& k) const
    {
        size_t h = std::hash<std::string>{}(k.text);
        h ^= std::hash<size_t>{}(k.fontId) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<uint32_t>{}((uint32_t(k.r) << 16) | (uint32_t(k.g) << 8) | k.b)
             + 0x9e3779b9 + (h << 6) + (h >> 2);
        return h;
    }
};

struct TextCacheEntry
{
    SDL_Texture* texture;
    float width;
    float height;
    uint64_t lastUsedFrame;
};

static std::unordered_map<TextCacheKey, TextCacheEntry, TextCacheKeyHash> f_textCaches[MAX_NUM_FRAMES];
static uint64_t f_renderFrameCounter = 0;


static Status renderClear(FrameID id, uint8_t r, uint8_t g, uint8_t b)
{
    SDL_Renderer* rend = getFrameRenderer(id);
    if(!rend)
    {
        LOG_ERROR(FRAME_LOG_ID, "No renderer for frame {}", id);
        return STATUS_ERROR_INVALID_PARAM;
    }

    if(!SDL_SetRenderDrawColor(rend, r, g, b, 255))
    {
        LOG_ERROR(FRAME_LOG_ID, "SDL failed to set draw color: {}", SDL_GetError());
        return STATUS_ERROR_LIB_CALL;
    }

    if(!SDL_RenderClear(rend))
    {
        LOG_ERROR(FRAME_LOG_ID, "SDL failed to clear renderer: {}", SDL_GetError());
        return STATUS_ERROR_LIB_CALL;
    }

    return STATUS_OK;
}


static Status renderBox(FrameID id, float x, float y, float w, float h, uint8_t r, uint8_t g, uint8_t b)
{
    SDL_Renderer* rend = getFrameRenderer(id);
    if(!rend)
    {
        LOG_ERROR(FRAME_LOG_ID, "No renderer for frame {}", id);
        return STATUS_ERROR_INVALID_PARAM;
    }

    if(!SDL_SetRenderDrawColor(rend, r, g, b, 255))
    {
        LOG_ERROR(FRAME_LOG_ID, "SDL failed to set draw color: {}", SDL_GetError());
        return STATUS_ERROR_LIB_CALL;
    }

    SDL_FRect rect = { x, y, w, h };
    if(!SDL_RenderFillRect(rend, &rect))
    {
        LOG_ERROR(FRAME_LOG_ID, "SDL failed to render box: {}", SDL_GetError());
        return STATUS_ERROR_LIB_CALL;
    }

    return STATUS_OK;
}


static constexpr int CIRCLE_SEGMENTS = 32;

static Status renderCircle(FrameID id, float cx, float cy, float radius, uint8_t r, uint8_t g, uint8_t b)
{
    SDL_Renderer* rend = getFrameRenderer(id);
    if(!rend)
    {
        LOG_ERROR(FRAME_LOG_ID, "No renderer for frame {}", id);
        return STATUS_ERROR_INVALID_PARAM;
    }

    // Triangle fan: center vertex + ring of outer vertices
    SDL_FColor color = {
        static_cast<float>(r) / 255.0f,
        static_cast<float>(g) / 255.0f,
        static_cast<float>(b) / 255.0f,
        1.0f
    };

    SDL_Vertex center;
    center.position  = { cx, cy };
    center.color     = color;
    center.tex_coord = { 0.0f, 0.0f };

    SDL_Vertex ring[CIRCLE_SEGMENTS];
    for(int i = 0; i < CIRCLE_SEGMENTS; i++)
    {
        float a = static_cast<float>(i) * (2.0f * static_cast<float>(M_PI) / static_cast<float>(CIRCLE_SEGMENTS));
        ring[i].position  = { cx + radius * std::cos(a), cy + radius * std::sin(a) };
        ring[i].color     = color;
        ring[i].tex_coord = { 0.0f, 0.0f };
    }

    // Build index buffer: CIRCLE_SEGMENTS triangles, each (center, ring[i], ring[i+1])
    int indices[CIRCLE_SEGMENTS * 3];
    // Vertex 0 = center, vertices 1..CIRCLE_SEGMENTS = ring
    for(int i = 0; i < CIRCLE_SEGMENTS; i++)
    {
        indices[i * 3 + 0] = 0;
        indices[i * 3 + 1] = i + 1;
        indices[i * 3 + 2] = (i + 1) % CIRCLE_SEGMENTS + 1;
    }

    SDL_Vertex verts[CIRCLE_SEGMENTS + 1];
    verts[0] = center;
    for(int i = 0; i < CIRCLE_SEGMENTS; i++)
        verts[i + 1] = ring[i];

    if(!SDL_RenderGeometry(rend, nullptr, verts, CIRCLE_SEGMENTS + 1, indices, CIRCLE_SEGMENTS * 3))
    {
        LOG_ERROR(FRAME_LOG_ID, "SDL failed to render circle: {}", SDL_GetError());
        return STATUS_ERROR_LIB_CALL;
    }

    return STATUS_OK;
}


// RenderShape implementation
Status RenderShape::render(FrameID frameId) const
{
    switch(m_type)
    {
        case ShapeType::Box:
            return renderBox(frameId,
                m_x, m_y, m_width, m_height,
                m_color.r, m_color.g, m_color.b);

        case ShapeType::Circle:
            return renderCircle(frameId,
                m_x, m_y, m_width * 0.5f,
                m_color.r, m_color.g, m_color.b);

        case ShapeType::Arc:
            LOG_ERROR(FRAME_LOG_ID, "Arc shapes must not be rendered directly — use a cached board texture");
            return STATUS_ERROR_INVALID_PARAM;
    }

    return STATUS_ERROR_INVALID_PARAM;
}


static Status renderText(FrameID id, const std::string& text, FontID fontId,
                          float x, float y, float rotation,
                          float scaleX, float scaleY,
                          uint8_t r, uint8_t g, uint8_t b)
{
    SDL_Renderer* rend = getFrameRenderer(id);
    if(!rend)
    {
        LOG_ERROR(FRAME_LOG_ID, "No renderer for frame {}", id);
        return STATUS_ERROR_INVALID_PARAM;
    }

    if(text.empty())
    {
        return STATUS_OK;
    }

    // Look up in text cache
    TextCacheKey key{text, fontId, r, g, b};
    auto& cache = f_textCaches[id];
    auto it = cache.find(key);

    SDL_Texture* texture = nullptr;
    float texW = 0.0f;
    float texH = 0.0f;

    if(it != cache.end())
    {
        // Cache hit
        texture = it->second.texture;
        texW    = it->second.width;
        texH    = it->second.height;
        it->second.lastUsedFrame = f_renderFrameCounter;
    }
    else
    {
        // Cache miss — rasterize and store
        TTF_Font* font = getFont(fontId);
        if(!font)
        {
            LOG_ERROR(FRAME_LOG_ID, "Invalid font ID: {}", fontId);
            return STATUS_ERROR_INVALID_PARAM;
        }

        SDL_Color color = { r, g, b, 255 };
        SDL_Surface* surface = TTF_RenderText_Blended(font, text.c_str(), 0, color);
        if(!surface)
        {
            LOG_ERROR(FRAME_LOG_ID, "Failed to render text surface: {}", SDL_GetError());
            return STATUS_ERROR_LIB_CALL;
        }

        texture = SDL_CreateTextureFromSurface(rend, surface);
        SDL_DestroySurface(surface);

        if(!texture)
        {
            LOG_ERROR(FRAME_LOG_ID, "Failed to create text texture: {}", SDL_GetError());
            return STATUS_ERROR_LIB_CALL;
        }

        SDL_GetTextureSize(texture, &texW, &texH);

        // Evict stale entries if cache is full
        if(cache.size() >= TEXT_CACHE_MAX)
        {
            for(auto cit = cache.begin(); cit != cache.end(); )
            {
                if(cit->second.lastUsedFrame < f_renderFrameCounter - 1)
                {
                    SDL_DestroyTexture(cit->second.texture);
                    cit = cache.erase(cit);
                }
                else
                {
                    ++cit;
                }
            }
        }

        cache[key] = TextCacheEntry{texture, texW, texH, f_renderFrameCounter};
    }

    SDL_FRect dstRect = { x, y, texW * scaleX, texH * scaleY };

    bool ok;
    if(rotation != 0.0f)
    {
        ok = SDL_RenderTextureRotated(rend, texture, nullptr, &dstRect, rotation, nullptr, SDL_FLIP_NONE);
    }
    else
    {
        ok = SDL_RenderTexture(rend, texture, nullptr, &dstRect);
    }

    if(!ok)
    {
        LOG_ERROR(FRAME_LOG_ID, "Failed to render text: {}", SDL_GetError());
        return STATUS_ERROR_LIB_CALL;
    }

    return STATUS_OK;
}


// RenderText implementation
Status RenderText::render(FrameID frameId) const
{
    return renderText(frameId, m_text, m_fontId,
        m_x, m_y, m_rotation, m_scaleX, m_scaleY,
        m_color.r, m_color.g, m_color.b);
}


// Render queue functions
Status renderQueueAdd(FrameID frameId, RenderObjectPtr obj)
{
    if(frameId >= MAX_NUM_FRAMES)
    {
        LOG_ERROR(FRAME_LOG_ID, "Invalid frame ID: {}", frameId);
        return STATUS_ERROR_INVALID_PARAM;
    }

    f_renderQueues[frameId].push_back(obj);
    return STATUS_OK;
}


Status renderQueueDrawFlush(FrameID frameId)
{
    if(frameId >= MAX_NUM_FRAMES)
    {
        LOG_ERROR(FRAME_LOG_ID, "Invalid frame ID: {}", frameId);
        return STATUS_ERROR_INVALID_PARAM;
    }

    f_renderFrameCounter++;

    for(const auto& cmd : f_renderQueues[frameId])
    {
        Status stat = cmd->render(frameId);
        if(IS_STATUS_NOT_OK(stat))
        {
            f_renderQueues[frameId].clear();
            return stat;
        }
    }

    f_renderQueues[frameId].clear();
    return STATUS_OK;
}


Status renderQueueDiscard(FrameID frameId)
{
    if(frameId >= MAX_NUM_FRAMES)
    {
        LOG_ERROR(FRAME_LOG_ID, "Invalid frame ID: {}", frameId);
        return STATUS_ERROR_INVALID_PARAM;
    }

    f_renderQueues[frameId].clear();
    return STATUS_OK;
}


Status renderQueueClearFrame(FrameID frameId, uint8_t r, uint8_t g, uint8_t b)
{
    if(frameId >= MAX_NUM_FRAMES)
    {
        LOG_ERROR(FRAME_LOG_ID, "Invalid frame ID: {}", frameId);
        return STATUS_ERROR_INVALID_PARAM;
    }

    return renderClear(frameId, r, g, b);
}


// RenderCachedTexture implementation
Status RenderCachedTexture::render(FrameID frameId) const
{
    if(!m_texture)
    {
        return STATUS_ERROR_NULL;
    }

    SDL_Renderer* rend = getFrameRenderer(frameId);
    if(!rend)
    {
        return STATUS_ERROR_INVALID_PARAM;
    }

    SDL_FRect dstRect = { m_x, m_y, m_width, m_height };
    if(!SDL_RenderTexture(rend, m_texture, nullptr, &dstRect))
    {
        LOG_ERROR(FRAME_LOG_ID, "Failed to render cached texture: {}", SDL_GetError());
        return STATUS_ERROR_LIB_CALL;
    }

    return STATUS_OK;
}


// RenderImage implementation
Status RenderImage::render(FrameID frameId) const
{
    if(m_imageId == INVALID_IMAGE_ID)
    {
        return STATUS_ERROR_NULL;
    }

    SDL_Texture* tex = getImageTexture(frameId, m_imageId);
    if(!tex)
    {
        return STATUS_ERROR_NULL;
    }

    float w = m_width;
    float h = m_height;
    if(w <= 0.0f || h <= 0.0f)
    {
        getImageSize(m_imageId, w, h);
    }

    SDL_Renderer* rend = getFrameRenderer(frameId);
    if(!rend)
    {
        return STATUS_ERROR_INVALID_PARAM;
    }

    SDL_FRect dstRect = { m_x, m_y, w, h };
    if(!SDL_RenderTexture(rend, tex, nullptr, &dstRect))
    {
        LOG_ERROR(FRAME_LOG_ID, "Failed to render image: {}", SDL_GetError());
        return STATUS_ERROR_LIB_CALL;
    }

    return STATUS_OK;
}


// ============================================================================
// Text cache cleanup
// ============================================================================

void clearTextCache(FrameID frameId)
{
    if(frameId >= MAX_NUM_FRAMES) return;

    for(auto& [key, entry] : f_textCaches[frameId])
    {
        SDL_DestroyTexture(entry.texture);
    }
    f_textCaches[frameId].clear();
}


void evictTextCacheByFont(FontID fontId)
{
    for(size_t i = 0; i < MAX_NUM_FRAMES; i++)
    {
        for(auto it = f_textCaches[i].begin(); it != f_textCaches[i].end(); )
        {
            if(it->first.fontId == fontId)
            {
                SDL_DestroyTexture(it->second.texture);
                it = f_textCaches[i].erase(it);
            }
            else
            {
                ++it;
            }
        }
    }
}
