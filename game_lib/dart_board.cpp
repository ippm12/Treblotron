/**
 * dart_board.cpp
 *
 * DartBoard factory implementation: creates 82 child entities with
 * computed arc geometry, dual palette colors, and z-ordering.
 */

#include "game_lib/entities/dart_board.hpp"
#include "game_lib/components/render_cached_texture.hpp"
#include "dart/dart_board_geometry.hpp"
#include "frame/render_queue.hpp"
#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>
#include <cmath>
#include <string>
#include <vector>

using namespace DartBoardGeometry;

// ── Default board palette (standard dart board colors) ──────────────
static constexpr Color DEFAULT_BOARD_PALETTE[] =
{
    { 227,  38,  54 },  // DoubleTripleA - Red
    {   0, 129,  55 },  // DoubleTripleB - Green
    { 255, 234, 196 },  // SingleA       - Cream
    {  30,  30,  30 },  // SingleB       - Black
    {   0, 129,  55 },  // OuterBull     - Green
    { 227,  38,  54 },  // InnerBull     - Red
    {  30,  30,  30 },  // Background    - Black (matches SingleB)
};

// ── Default highlight palette (brighter/lighter versions) ───────────
static constexpr Color DEFAULT_HIGHLIGHT_PALETTE[] =
{
    { 255, 100, 100 },  // DoubleTripleA - Bright Red
    { 100, 255, 100 },  // DoubleTripleB - Bright Green
    { 255, 255, 255 },  // SingleA       - White
    { 120, 120, 120 },  // SingleB       - Gray
    { 100, 255, 100 },  // OuterBull     - Bright Green
    { 255, 100, 100 },  // InnerBull     - Bright Red
    { 120, 120, 120 },  // Background    - Gray
};

// ── Default dim palette (desaturated grays, alternating for boundary visibility) ──
static constexpr Color DEFAULT_DIM_PALETTE[] =
{
    {  55,  55,  55 },  // DoubleTripleA - Dark gray  (was Red)
    {  42,  42,  42 },  // DoubleTripleB - Darker gray (was Green)
    {  75,  75,  75 },  // SingleA       - Medium gray (was Cream)
    {  28,  28,  28 },  // SingleB       - Very dark gray (was Black)
    {  42,  42,  42 },  // OuterBull     - Darker gray (was Green)
    {  55,  55,  55 },  // InnerBull     - Dark gray  (was Red)
    {  20,  20,  20 },  // Background    - Dim Black
};

// ── Z-order offsets from configurable base (inner layers render on top) ──
static constexpr uint32_t Z_OFF_BACKGROUND   = 0;
static constexpr uint32_t Z_OFF_DOUBLE       = 1;
static constexpr uint32_t Z_OFF_OUTER_SINGLE = 2;
static constexpr uint32_t Z_OFF_TRIPLE       = 3;
static constexpr uint32_t Z_OFF_INNER_SINGLE = 4;
static constexpr uint32_t Z_OFF_OUTER_BULL   = 5;
static constexpr uint32_t Z_OFF_INNER_BULL   = 6;
static constexpr uint32_t Z_OFF_LABELS       = 7;

// ── Background circle ──────────────────────────────────────────────

// ── Label constants ─────────────────────────────────────────────────
// Radius proportion for label placement (just outside the double ring)
static constexpr float RADIUS_LABEL = 1.08f;


static constexpr Color DEFAULT_LABEL_COLOR = { 255, 255, 255 };


struct RingGeometry
{
    float    outerRadius; // Proportion of board edge
    float    innerRadius; // Proportion of board edge
    uint32_t zOrder;
};


static RingGeometry getRingGeometry(DartRing ring, uint32_t zBase)
{
    switch(ring)
    {
        case DartRing::Double:
            return { RADIUS_DOUBLE_OUTER, RADIUS_DOUBLE_INNER, zBase + Z_OFF_DOUBLE };
        case DartRing::OuterSingle:
            return { RADIUS_DOUBLE_INNER, RADIUS_OUTER_SINGLE_INNER, zBase + Z_OFF_OUTER_SINGLE };
        case DartRing::Triple:
            return { RADIUS_OUTER_SINGLE_INNER, RADIUS_TRIPLE_INNER, zBase + Z_OFF_TRIPLE };
        case DartRing::InnerSingle:
            return { RADIUS_TRIPLE_INNER, RADIUS_INNER_SINGLE_INNER, zBase + Z_OFF_INNER_SINGLE };
        case DartRing::OuterBull:
            return { RADIUS_INNER_SINGLE_INNER, RADIUS_OUTER_BULL_INNER, zBase + Z_OFF_OUTER_BULL };
        case DartRing::InnerBull:
            return { RADIUS_OUTER_BULL_INNER, 0.0f, zBase + Z_OFF_INNER_BULL };
        default:
            return { 0.0f, 0.0f, zBase };
    }
}


BoardColor DartBoard::getSegmentRole(DartSegment segment)
{
    DartRing ring = getSegmentRing(segment);

    if(ring == DartRing::OuterBull)
    {
        return BoardColor::OuterBull;
    }
    if(ring == DartRing::InnerBull)
    {
        return BoardColor::InnerBull;
    }

    uint8_t section  = getSegmentSection(segment);
    uint8_t posIndex = getSectionPositionIndex(section);
    // Section 20 sits at posIndex 0 and must render dark (B); section 1 at
    // posIndex 1 must render light (A). Odd positions are the light role.
    bool isLight     = (posIndex % 2) == 1;

    if(ring == DartRing::Double || ring == DartRing::Triple)
    {
        return isLight ? BoardColor::DoubleTripleA : BoardColor::DoubleTripleB;
    }

    // OuterSingle or InnerSingle
    return isLight ? BoardColor::SingleA : BoardColor::SingleB;
}


// ============================================================================
// Constructor / Destructor / Move
// ============================================================================

DartBoard::DartBoard()
    : m_labelColor(DEFAULT_LABEL_COLOR)
{
    for(size_t i = 0; i < NUM_SEGMENTS; i++)
    {
        m_highlighted[i] = false;
        m_dimmed[i] = false;
    }
}

DartBoard::~DartBoard()
{
    if(m_cachedTexture)
    {
        SDL_DestroyTexture(m_cachedTexture);
        m_cachedTexture = nullptr;
    }
}

DartBoard::DartBoard(DartBoard&& other) noexcept
    : m_root(other.m_root)
    , m_background(other.m_background)
    , m_labelColor(other.m_labelColor)
    , m_labelsVisible(other.m_labelsVisible)
    , m_fontId(other.m_fontId)
    , m_centerX(other.m_centerX)
    , m_centerY(other.m_centerY)
    , m_scale(other.m_scale)
    , m_zBase(other.m_zBase)
    , m_cachedTexture(other.m_cachedTexture)
    , m_textureDirty(other.m_textureDirty)
    , m_texW(other.m_texW)
    , m_texH(other.m_texH)
    , m_texOffsetX(other.m_texOffsetX)
    , m_texOffsetY(other.m_texOffsetY)
{
    for(size_t i = 0; i < PALETTE_SIZE; i++)
    {
        m_boardPalette[i]     = other.m_boardPalette[i];
        m_highlightPalette[i] = other.m_highlightPalette[i];
        m_dimPalette[i]       = other.m_dimPalette[i];
    }
    for(size_t i = 0; i < NUM_SEGMENTS; i++)
    {
        m_segments[i]    = other.m_segments[i];
        m_highlighted[i] = other.m_highlighted[i];
        m_dimmed[i]      = other.m_dimmed[i];
    }
    for(size_t i = 0; i < NUM_LABELS; i++)
        m_labels[i] = other.m_labels[i];

    other.m_cachedTexture = nullptr;
}

DartBoard& DartBoard::operator=(DartBoard&& other) noexcept
{
    if(this != &other)
    {
        if(m_cachedTexture)
            SDL_DestroyTexture(m_cachedTexture);

        m_root           = other.m_root;
        m_background     = other.m_background;
        m_labelColor     = other.m_labelColor;
        m_labelsVisible  = other.m_labelsVisible;
        m_fontId         = other.m_fontId;
        m_centerX        = other.m_centerX;
        m_centerY        = other.m_centerY;
        m_scale          = other.m_scale;
        m_zBase          = other.m_zBase;
        m_cachedTexture  = other.m_cachedTexture;
        m_textureDirty   = other.m_textureDirty;
        m_texW           = other.m_texW;
        m_texH           = other.m_texH;
        m_texOffsetX     = other.m_texOffsetX;
        m_texOffsetY     = other.m_texOffsetY;

        for(size_t i = 0; i < PALETTE_SIZE; i++)
        {
            m_boardPalette[i]     = other.m_boardPalette[i];
            m_highlightPalette[i] = other.m_highlightPalette[i];
            m_dimPalette[i]       = other.m_dimPalette[i];
        }
        for(size_t i = 0; i < NUM_SEGMENTS; i++)
        {
            m_segments[i]    = other.m_segments[i];
            m_highlighted[i] = other.m_highlighted[i];
            m_dimmed[i]      = other.m_dimmed[i];
        }
        for(size_t i = 0; i < NUM_LABELS; i++)
            m_labels[i] = other.m_labels[i];

        other.m_cachedTexture = nullptr;
    }
    return *this;
}


// ============================================================================
// Triangle-fan arc rendering (for texture rebuild)
// ============================================================================

static constexpr float ARC_STEP_DEG = 3.0f;

static void renderArcToTarget(SDL_Renderer* rend, float cx, float cy,
                               float outerR, float innerR,
                               float startDeg, float endDeg,
                               uint8_t r, uint8_t g, uint8_t b)
{
    float startRad = static_cast<float>(startDeg * M_PI / 180.0);
    float endRad   = static_cast<float>(endDeg * M_PI / 180.0);
    while(endRad < startRad) endRad += static_cast<float>(2.0 * M_PI);

    float spanRad = endRad - startRad;
    int steps = std::max(1, static_cast<int>(std::ceil(spanRad / static_cast<float>(ARC_STEP_DEG * M_PI / 180.0))));

    float stepRad = spanRad / static_cast<float>(steps);

    // Build vertices: for each angular step, one outer + one inner vertex
    std::vector<SDL_Vertex> verts;
    verts.reserve(static_cast<size_t>((steps + 1) * 2));

    SDL_FColor color = {
        static_cast<float>(r) / 255.0f,
        static_cast<float>(g) / 255.0f,
        static_cast<float>(b) / 255.0f,
        1.0f
    };

    for(int i = 0; i <= steps; i++)
    {
        float a = startRad + static_cast<float>(i) * stepRad;
        float cosA = std::cos(a);
        float sinA = std::sin(a);

        SDL_Vertex outer;
        outer.position = { cx + outerR * cosA, cy + outerR * sinA };
        outer.color    = color;
        outer.tex_coord = { 0.0f, 0.0f };
        verts.push_back(outer);

        SDL_Vertex inner;
        inner.position = { cx + innerR * cosA, cy + innerR * sinA };
        inner.color    = color;
        inner.tex_coord = { 0.0f, 0.0f };
        verts.push_back(inner);
    }

    // Build index buffer: 2 triangles per step (quad strip)
    std::vector<int> indices;
    indices.reserve(static_cast<size_t>(steps * 6));

    for(int i = 0; i < steps; i++)
    {
        int base = i * 2;
        // Triangle 1: outer_i, inner_i, outer_i+1
        indices.push_back(base);
        indices.push_back(base + 1);
        indices.push_back(base + 2);
        // Triangle 2: inner_i, inner_i+1, outer_i+1
        indices.push_back(base + 1);
        indices.push_back(base + 3);
        indices.push_back(base + 2);
    }

    SDL_RenderGeometry(rend, nullptr,
                       verts.data(), static_cast<int>(verts.size()),
                       indices.data(), static_cast<int>(indices.size()));
}


static void renderCircleToTarget(SDL_Renderer* rend, float cx, float cy,
                                  float radius, uint8_t r, uint8_t g, uint8_t b)
{
    // Full circle as a triangle fan
    renderArcToTarget(rend, cx, cy, radius, 0.0f, 0.0f, 360.0f, r, g, b);
}


// ============================================================================
// Texture cache rebuild
// ============================================================================

void DartBoard::rebuildTexture(FrameID frameId)
{
    SDL_Renderer* rend = getFrameRenderer(frameId);
    if(!rend) return;

    float scaledRadius = BASE_RADIUS * m_scale;
    float bgRadius = RADIUS_BACKGROUND * scaledRadius;

    m_texW       = bgRadius * 2.0f;
    m_texH       = bgRadius * 2.0f;
    m_texOffsetX = m_centerX - bgRadius;
    m_texOffsetY = m_centerY - bgRadius;

    int texWi = static_cast<int>(std::ceil(m_texW));
    int texHi = static_cast<int>(std::ceil(m_texH));

    // Recreate texture if size changed or first time
    if(m_cachedTexture)
    {
        SDL_DestroyTexture(m_cachedTexture);
        m_cachedTexture = nullptr;
    }

    m_cachedTexture = SDL_CreateTexture(rend, SDL_PIXELFORMAT_RGBA8888,
                                         SDL_TEXTUREACCESS_TARGET, texWi, texHi);
    if(!m_cachedTexture)
    {
        LOG_ERROR(GAME_MANAGER_LOG_ID, "Failed to create board cache texture: {}", SDL_GetError());
        return;
    }

    SDL_SetTextureBlendMode(m_cachedTexture, SDL_BLENDMODE_BLEND);

    // Save and switch render target
    SDL_Texture* prevTarget = SDL_GetRenderTarget(rend);
    SDL_SetRenderTarget(rend, m_cachedTexture);

    // Clear to transparent
    SDL_SetRenderDrawColor(rend, 0, 0, 0, 0);
    SDL_RenderClear(rend);

    // Local center in texture space
    float localCx = bgRadius;
    float localCy = bgRadius;

    // Render background circle
    if(m_background.is_valid())
    {
        const RenderShape& bg = m_background.get<RenderShape>();
        float radius = bg.m_width * 0.5f;
        renderCircleToTarget(rend, localCx, localCy, radius,
                             bg.m_color.r, bg.m_color.g, bg.m_color.b);
    }

    // Render all 82 segments as arcs (z-order handled by draw order: outer to inner)
    // We render in z-order so inner rings paint over outer rings
    struct SegmentDraw { uint32_t z; size_t idx; };
    std::vector<SegmentDraw> sortedSegs;
    sortedSegs.reserve(NUM_SEGMENTS);

    for(size_t i = 0; i < NUM_SEGMENTS; i++)
    {
        if(!m_segments[i].is_valid()) continue;
        const RenderShape& s = m_segments[i].get<RenderShape>();
        sortedSegs.push_back({s.m_z, i});
    }

    // Sort by z-order ascending (lower z drawn first = behind)
    std::sort(sortedSegs.begin(), sortedSegs.end(),
              [](const SegmentDraw& a, const SegmentDraw& b) { return a.z < b.z; });

    for(const auto& sd : sortedSegs)
    {
        const RenderShape& s = m_segments[sd.idx].get<RenderShape>();
        renderArcToTarget(rend, localCx, localCy,
                          s.m_width, s.m_height,
                          s.m_startAngle, s.m_endAngle,
                          s.m_color.r, s.m_color.g, s.m_color.b);
    }

    // Restore previous render target
    SDL_SetRenderTarget(rend, prevTarget);

    m_textureDirty = false;
}


// ============================================================================
// Factory
// ============================================================================

DartBoard DartBoard::create(flecs::world& world, float centerX, float centerY, float scale, FontID fontId, uint32_t zBase)
{
    DartBoard board;
    board.m_centerX = centerX;
    board.m_centerY = centerY;
    board.m_scale   = scale;
    board.m_fontId  = fontId;
    board.m_zBase   = zBase;
    board.m_labelColor   = DEFAULT_LABEL_COLOR;
    board.m_labelsVisible = true;

    // Initialize palettes
    for(size_t i = 0; i < PALETTE_SIZE; i++)
    {
        board.m_boardPalette[i]     = DEFAULT_BOARD_PALETTE[i];
        board.m_highlightPalette[i] = DEFAULT_HIGHLIGHT_PALETTE[i];
        board.m_dimPalette[i]       = DEFAULT_DIM_PALETTE[i];
    }

    // All segments start unhighlighted and undimmed
    for(size_t i = 0; i < NUM_SEGMENTS; i++)
    {
        board.m_highlighted[i] = false;
        board.m_dimmed[i]      = false;
    }

    board.m_root = world.entity("DartBoard");

    float scaledRadius = BASE_RADIUS * scale;

    // Background circle (always rendered behind all segments)
    RenderShape bgShape;
    bgShape.m_type  = ShapeType::Circle;
    bgShape.m_color = board.m_boardPalette[static_cast<size_t>(BoardColor::Background)];
    bgShape.m_x     = centerX;
    bgShape.m_y     = centerY;
    bgShape.m_z     = zBase + Z_OFF_BACKGROUND;
    bgShape.m_width = 2.0f * RADIUS_BACKGROUND * scaledRadius;
    board.m_background = world.entity()
        .child_of(board.m_root)
        .set<RenderShape>(bgShape);

    for(uint8_t segIdx = 0; segIdx < DART_NUM_SEGMENTS; segIdx++)
    {
        DartSegment segment = static_cast<DartSegment>(segIdx);
        DartRing    ring    = getSegmentRing(segment);
        RingGeometry geom   = getRingGeometry(ring, zBase);
        BoardColor  role    = getSegmentRole(segment);

        float outerR = geom.outerRadius * scaledRadius;
        float innerR = geom.innerRadius * scaledRadius;

        RenderShape shape;
        shape.m_z     = geom.zOrder;
        shape.m_color = board.m_boardPalette[static_cast<size_t>(role)];

        uint8_t section = getSegmentSection(segment);

        if(ring == DartRing::OuterBull || ring == DartRing::InnerBull)
        {
            shape.m_type       = ShapeType::Arc;
            shape.m_x          = centerX;
            shape.m_y          = centerY;
            shape.m_width      = outerR;
            shape.m_height     = innerR;
            shape.m_startAngle = 0.0f;
            shape.m_endAngle   = 360.0f;
        }
        else
        {
            uint8_t posIdx = getSectionPositionIndex(section);

            float sectionCenter = static_cast<float>(posIdx) * 18.0f - 90.0f;
            float startAngle    = sectionCenter - 9.0f;
            float endAngle      = sectionCenter + 9.0f;

            shape.m_type       = ShapeType::Arc;
            shape.m_x          = centerX;
            shape.m_y          = centerY;
            shape.m_width      = outerR;
            shape.m_height     = innerR;
            shape.m_startAngle = startAngle;
            shape.m_endAngle   = endAngle;
        }

        board.m_segments[segIdx] = world.entity()
            .child_of(board.m_root)
            .set<RenderShape>(shape);
    }

    // Create section number labels
    const uint8_t* layout = getBoardLayout();
    float labelRadius = RADIUS_LABEL * scaledRadius;
    TTF_Font* font = getFont(fontId);

    for(uint8_t posIdx = 0; posIdx < DART_NUM_SECTIONS; posIdx++)
    {
        float angleDeg = static_cast<float>(posIdx) * 18.0f - 90.0f;
        float angleRad = static_cast<float>(angleDeg * M_PI / 180.0);

        std::string labelStr = std::to_string(layout[posIdx]);

        // Get actual text dimensions from the font, then apply scale
        int textW = 0;
        int textH = 0;
        TTF_GetStringSize(font, labelStr.c_str(), 0, &textW, &textH);
        float halfW = static_cast<float>(textW) * scale * 0.5f;
        float halfH = static_cast<float>(textH) * scale * 0.5f;

        // Position top-left so the rect center lands on the radial point.
        // SDL rotates around the rect center, so this keeps labels centered
        // regardless of rotation and character count.
        float labelX = centerX + std::cos(angleRad) * labelRadius - halfW;
        float labelY = centerY + std::sin(angleRad) * labelRadius - halfH;

        RenderText text;
        text.m_text     = labelStr;
        text.m_color    = DEFAULT_LABEL_COLOR;
        text.m_fontId   = fontId;
        text.m_rotation = static_cast<float>(posIdx) * 18.0f;
        text.m_scaleX   = scale;
        text.m_scaleY   = scale;
        text.m_x        = labelX;
        text.m_y        = labelY;
        text.m_z        = zBase + Z_OFF_LABELS;

        board.m_labels[posIdx] = world.entity()
            .child_of(board.m_root)
            .set<RenderText>(text);
    }

    return board;
}


void DartBoard::setSegmentColor(DartSegment segment, Color color)
{
    size_t idx = static_cast<size_t>(segment);
    if(idx >= NUM_SEGMENTS || !m_segments[idx].is_valid())
    {
        return;
    }

    RenderShape& s = m_segments[idx].ensure<RenderShape>();
    s.m_color = color;
    m_textureDirty = true;
}


void DartBoard::setBoardColor(BoardColor role, Color color)
{
    size_t roleIdx = static_cast<size_t>(role);
    if(roleIdx >= PALETTE_SIZE)
    {
        return;
    }

    m_boardPalette[roleIdx] = color;

    // Update background entity if that's the role being changed
    if(role == BoardColor::Background && m_background.is_valid())
    {
        RenderShape& bg = m_background.ensure<RenderShape>();
        bg.m_color = color;
        m_textureDirty = true;
    }

    // Update all unhighlighted segments with this role
    for(size_t i = 0; i < NUM_SEGMENTS; i++)
    {
        if(!m_highlighted[i] && getSegmentRole(static_cast<DartSegment>(i)) == role)
        {
            setSegmentColor(static_cast<DartSegment>(i), color);
        }
    }
}


void DartBoard::setHighlightColor(BoardColor role, Color color)
{
    size_t roleIdx = static_cast<size_t>(role);
    if(roleIdx >= PALETTE_SIZE)
    {
        return;
    }

    m_highlightPalette[roleIdx] = color;

    // Update all highlighted segments with this role
    for(size_t i = 0; i < NUM_SEGMENTS; i++)
    {
        if(m_highlighted[i] && getSegmentRole(static_cast<DartSegment>(i)) == role)
        {
            setSegmentColor(static_cast<DartSegment>(i), color);
        }
    }
}


void DartBoard::highlightSegment(DartSegment segment)
{
    size_t idx = static_cast<size_t>(segment);
    if(idx >= NUM_SEGMENTS)
    {
        return;
    }

    m_highlighted[idx] = true;
    BoardColor role = getSegmentRole(segment);
    setSegmentColor(segment, m_highlightPalette[static_cast<size_t>(role)]);
}


void DartBoard::unhighlightSegment(DartSegment segment)
{
    size_t idx = static_cast<size_t>(segment);
    if(idx >= NUM_SEGMENTS)
    {
        return;
    }

    m_highlighted[idx] = false;
    BoardColor role = getSegmentRole(segment);
    size_t roleIdx = static_cast<size_t>(role);
    setSegmentColor(segment, m_dimmed[idx] ? m_dimPalette[roleIdx] : m_boardPalette[roleIdx]);
}


void DartBoard::unhighlightAll()
{
    for(size_t i = 0; i < NUM_SEGMENTS; i++)
    {
        m_highlighted[i] = false;
        BoardColor role = getSegmentRole(static_cast<DartSegment>(i));
        size_t roleIdx = static_cast<size_t>(role);
        setSegmentColor(static_cast<DartSegment>(i),
                        m_dimmed[i] ? m_dimPalette[roleIdx] : m_boardPalette[roleIdx]);
    }
}


void DartBoard::setDimColor(BoardColor role, Color color)
{
    size_t roleIdx = static_cast<size_t>(role);
    if(roleIdx >= PALETTE_SIZE)
    {
        return;
    }

    m_dimPalette[roleIdx] = color;

    // Update all dimmed, unhighlighted segments with this role
    for(size_t i = 0; i < NUM_SEGMENTS; i++)
    {
        if(m_dimmed[i] && !m_highlighted[i] && getSegmentRole(static_cast<DartSegment>(i)) == role)
        {
            setSegmentColor(static_cast<DartSegment>(i), color);
        }
    }
}


void DartBoard::dimSegment(DartSegment segment)
{
    size_t idx = static_cast<size_t>(segment);
    if(idx >= NUM_SEGMENTS)
    {
        return;
    }

    m_dimmed[idx] = true;
    if(!m_highlighted[idx])
    {
        BoardColor role = getSegmentRole(segment);
        setSegmentColor(segment, m_dimPalette[static_cast<size_t>(role)]);
    }
}


void DartBoard::undimSegment(DartSegment segment)
{
    size_t idx = static_cast<size_t>(segment);
    if(idx >= NUM_SEGMENTS)
    {
        return;
    }

    m_dimmed[idx] = false;
    if(!m_highlighted[idx])
    {
        BoardColor role = getSegmentRole(segment);
        setSegmentColor(segment, m_boardPalette[static_cast<size_t>(role)]);
    }
}


void DartBoard::undimAll()
{
    for(size_t i = 0; i < NUM_SEGMENTS; i++)
    {
        m_dimmed[i] = false;
        if(!m_highlighted[i])
        {
            BoardColor role = getSegmentRole(static_cast<DartSegment>(i));
            setSegmentColor(static_cast<DartSegment>(i), m_boardPalette[static_cast<size_t>(role)]);
        }
    }
}


void DartBoard::showLabels(bool visible)
{
    m_labelsVisible = visible;
}


void DartBoard::setLabelColor(Color color)
{
    m_labelColor = color;

    for(size_t i = 0; i < NUM_LABELS; i++)
    {
        if(!m_labels[i].is_valid())
        {
            continue;
        }

        RenderText& t = m_labels[i].ensure<RenderText>();
        t.m_color = color;
    }
}


void DartBoard::enqueueRender(FrameID frameId)
{
    // Rebuild the cached texture if anything changed
    if(m_textureDirty || !m_cachedTexture)
    {
        rebuildTexture(frameId);
    }

    // Enqueue the single cached board texture
    if(m_cachedTexture)
    {
        auto ptr = std::make_shared<RenderCachedTexture>();
        ptr->m_texture = m_cachedTexture;
        ptr->m_x       = m_texOffsetX;
        ptr->m_y       = m_texOffsetY;
        ptr->m_z       = m_zBase;
        ptr->m_width   = m_texW;
        ptr->m_height  = m_texH;
        renderQueueAdd(frameId, ptr);
    }

    // Labels are still rendered as individual text objects (cached by text cache)
    if(m_labelsVisible)
    {
        for(size_t i = 0; i < NUM_LABELS; i++)
        {
            if(!m_labels[i].is_valid())
            {
                continue;
            }

            const RenderText& text = m_labels[i].get<RenderText>();
            auto ptr = std::make_shared<RenderText>(text);
            renderQueueAdd(frameId, ptr);
        }
    }
}


void DartBoard::setPosition(float centerX, float centerY)
{
    float dx = centerX - m_centerX;
    float dy = centerY - m_centerY;
    m_centerX = centerX;
    m_centerY = centerY;
    m_textureDirty = true;

    if(m_background.is_valid())
    {
        RenderShape& bg = m_background.ensure<RenderShape>();
        bg.m_x += dx;
        bg.m_y += dy;
    }

    for(size_t i = 0; i < NUM_SEGMENTS; i++)
    {
        if(!m_segments[i].is_valid())
        {
            continue;
        }

        RenderShape& s = m_segments[i].ensure<RenderShape>();
        s.m_x += dx;
        s.m_y += dy;
    }

    for(size_t i = 0; i < NUM_LABELS; i++)
    {
        if(!m_labels[i].is_valid())
        {
            continue;
        }

        RenderText& t = m_labels[i].ensure<RenderText>();
        t.m_x += dx;
        t.m_y += dy;
    }
}
