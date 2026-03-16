/**
 * dart_board.hpp
 *
 * DartBoard entity: a scalable dart board composed of 82 individually
 * addressable segments. Each segment is a Flecs child entity with a
 * RenderShape component. Segments can be colored via a triple palette
 * system (board palette + dim palette + highlight palette).
 */

#ifndef DART_BOARD_HPP
#define DART_BOARD_HPP

#include "common_inc.hpp"
#include "dart/dart_defs.hpp"
#include "game_lib/components/render_object.hpp"
#include "game_lib/components/render_shape.hpp"
#include "game_lib/components/render_text.hpp"
#include "flecs.h"

struct SDL_Texture;

/**
 * Color roles used by both the board palette and highlight palette.
 * Each segment maps to one of these roles based on its ring type
 * and position parity.
 */
enum class BoardColor : uint8_t
{
    DoubleTripleA = 0,  // Even-position doubles & triples
    DoubleTripleB,      // Odd-position doubles & triples
    SingleA,            // Even-position singles
    SingleB,            // Odd-position singles
    OuterBull,          // Outer bull
    InnerBull,          // Inner bull
    Background,         // Background circle behind all segments
    COUNT
};

class DartBoard
{
    public:
        static constexpr size_t PALETTE_SIZE = static_cast<size_t>(BoardColor::COUNT);

        /**
         * Create a dart board entity in the given world.
         * @param world     Flecs world to create entities in.
         * @param centerX   Horizontal center position in pixels.
         * @param centerY   Vertical center position in pixels.
         * @param scale     Size multiplier (1.0 = 400px diameter board).
         * @param fontId    Font to use for section number labels.
         * @param zBase     Base z-order for layering (default 0). Segments use zBase+0 through zBase+6.
         */
        static DartBoard create(flecs::world& world, float centerX, float centerY, float scale, FontID fontId, uint32_t zBase = 0);

        /** Set a board (unhighlighted) palette color for a role. Updates affected unhighlighted segments. */
        void setBoardColor(BoardColor role, Color color);

        /** Set a highlight palette color for a role. Updates affected highlighted segments. */
        void setHighlightColor(BoardColor role, Color color);

        /** Set a dim palette color for a role. Updates affected dimmed segments. */
        void setDimColor(BoardColor role, Color color);

        /** Highlight a segment (applies highlight palette color for its role). */
        void highlightSegment(DartSegment segment);

        /** Unhighlight a segment (restores board or dim palette color for its role). */
        void unhighlightSegment(DartSegment segment);

        /** Unhighlight all segments. */
        void unhighlightAll();

        /** Dim a segment (applies dim palette color for its role). */
        void dimSegment(DartSegment segment);

        /** Undim a segment (restores board palette color if not highlighted). */
        void undimSegment(DartSegment segment);

        /** Undim all segments. */
        void undimAll();

        /** Set a segment to a specific color, bypassing all palettes. */
        void setSegmentColor(DartSegment segment, Color color);

        /** Enqueue all segment shapes to the render queue for drawing. */
        void enqueueRender(FrameID frameId);

        /** Move the entire board to a new center position. */
        void setPosition(float centerX, float centerY);

        /** Show or hide the section number labels. */
        void showLabels(bool visible);

        /** Set the color of all section number labels. */
        void setLabelColor(Color color);

        DartBoard();
        ~DartBoard();

        // Move only — texture is a non-copyable GPU resource
        DartBoard(DartBoard&& other) noexcept;
        DartBoard& operator=(DartBoard&& other) noexcept;
        DartBoard(const DartBoard&) = delete;
        DartBoard& operator=(const DartBoard&) = delete;

    private:
        void rebuildTexture(FrameID frameId);

        static constexpr size_t NUM_SEGMENTS = static_cast<size_t>(DartSegment::COUNT);

        /** Returns which BoardColor role applies to a given segment. */
        static BoardColor getSegmentRole(DartSegment segment);

        flecs::entity m_root;
        flecs::entity m_background;
        flecs::entity m_segments[NUM_SEGMENTS];
        Color         m_boardPalette[PALETTE_SIZE];
        Color         m_highlightPalette[PALETTE_SIZE];
        Color         m_dimPalette[PALETTE_SIZE];
        bool          m_highlighted[NUM_SEGMENTS];
        bool          m_dimmed[NUM_SEGMENTS];
        static constexpr size_t NUM_LABELS = DART_NUM_SECTIONS; // 20

        flecs::entity m_labels[NUM_LABELS];
        Color         m_labelColor;
        bool          m_labelsVisible = true;
        FontID        m_fontId        = INVALID_FONT_ID;
        float         m_centerX       = 0.0f;
        float         m_centerY       = 0.0f;
        float         m_scale         = 1.0f;
        uint32_t      m_zBase         = 0;

        // Cached board texture (all shapes pre-rendered)
        SDL_Texture*  m_cachedTexture = nullptr;
        bool          m_textureDirty  = true;
        float         m_texW          = 0.0f;
        float         m_texH          = 0.0f;
        float         m_texOffsetX    = 0.0f;
        float         m_texOffsetY    = 0.0f;
};

#endif // DART_BOARD_HPP
