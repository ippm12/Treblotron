/**
 * sim_vision_source.cpp
 *
 * Simulated vision source implementation. Opens a clickable dart board
 * window and translates mouse clicks into dart detection callbacks.
 */

#include "sim_vision_source.hpp"
#include "dart/dart_board_geometry.hpp"
#include "frame/frame.hpp"
#include "frame/render_queue.hpp"
#include "game_lib/components/render_shape.hpp"
#include "game_lib/components/render_text.hpp"
#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

using namespace DartBoardGeometry;

// ── Window layout constants ─────────────────────────────────────────
static constexpr size_t SIM_WINDOW_WIDTH  = 600;
static constexpr size_t SIM_WINDOW_HEIGHT = 650;
static constexpr float  SIM_BOARD_CENTER_X = 300.0f;
static constexpr float  SIM_BOARD_CENTER_Y = 280.0f;
static constexpr float  SIM_BOARD_SCALE    = 1.1f;
static constexpr float  SIM_FONT_SIZE      = 18.0f;

// Collect button
static constexpr float  BTN_WIDTH  = 200.0f;
static constexpr float  BTN_HEIGHT = 40.0f;
static constexpr float  BTN_X      = (SIM_WINDOW_WIDTH - BTN_WIDTH) / 2.0f;
static constexpr float  BTN_Y      = SIM_WINDOW_HEIGHT - BTN_HEIGHT - 10.0f;

// Dart marker
static constexpr float    MARKER_DIAMETER = 8.0f;
static constexpr uint32_t MARKER_Z        = 1000;
static constexpr uint32_t BTN_Z           = 2000;


SimVisionSource::SimVisionSource()
{
}


SimVisionSource::~SimVisionSource()
{
}


Status SimVisionSource::init()
{
    // Create the sim window
    Status stat = createNewFrame("DartLens Simulator", SIM_WINDOW_WIDTH, SIM_WINDOW_HEIGHT, m_frameId);
    if(IS_STATUS_NOT_OK(stat))
    {
        LOG_ERROR(VISION_LOG_ID, "Failed to create sim window");
        return stat;
    }

    // Load font for button text and board labels
    m_fontId = loadFont("assets/fonts/Roboto-Regular.ttf", SIM_FONT_SIZE);
    if(m_fontId == INVALID_FONT_ID)
    {
        LOG_ERROR(VISION_LOG_ID, "Failed to load sim font");
        deleteFrame(m_frameId);
        m_frameId = INVALID_FRAME_ID;
        return STATUS_ERROR_GENERIC;
    }

    // Create the dart board in the sim's own ECS world
    m_boardCenterX = SIM_BOARD_CENTER_X;
    m_boardCenterY = SIM_BOARD_CENTER_Y;
    m_boardScale   = SIM_BOARD_SCALE;
    m_board = DartBoard::create(m_world, m_boardCenterX, m_boardCenterY, m_boardScale, m_fontId);

    // Store button geometry
    m_collectBtnX      = BTN_X;
    m_collectBtnY      = BTN_Y;
    m_collectBtnWidth  = BTN_WIDTH;
    m_collectBtnHeight = BTN_HEIGHT;

    // Register click handler
    registerFrameClickHandler(m_frameId, [this](FrameID fid, float x, float y, uint8_t btn)
    {
        onBoardClicked(fid, x, y, btn);
    });

    LOG_INFO(VISION_LOG_ID, "Sim vision source initialized (delay: {}ms)", m_positionDelay * 1000.0f);
    return STATUS_OK;
}


void SimVisionSource::tick(float deltaTime)
{
    // Process pending throws
    for(auto it = m_pendingThrows.begin(); it != m_pendingThrows.end(); )
    {
        it->delayRemaining -= deltaTime;
        if(it->delayRemaining <= 0.0f)
        {
            // Fire position callback
            if(m_onDartPositionCalculated)
            {
                m_onDartPositionCalculated(it->angle, it->normalizedRadius);
            }

            // Move to visual markers
            m_markers.push_back({ it->clickX, it->clickY });

            it = m_pendingThrows.erase(it);
        }
        else
        {
            ++it;
        }
    }

    // Render the sim window
    render();
}


void SimVisionSource::shutdown()
{
    if(m_frameId != INVALID_FRAME_ID)
    {
        unregisterFrameClickHandler(m_frameId);

        if(m_fontId != INVALID_FONT_ID)
        {
            unloadFont(m_fontId);
            m_fontId = INVALID_FONT_ID;
        }

        deleteFrame(m_frameId);
        m_frameId = INVALID_FRAME_ID;
    }

    LOG_INFO(VISION_LOG_ID, "Sim vision source shut down");
}


void SimVisionSource::onBoardClicked(FrameID /* frameId */, float mouseX, float mouseY, uint8_t button)
{
    // Only handle left clicks
    if(button != 1) // SDL_BUTTON_LEFT
    {
        return;
    }

    // Check if click is on the "Collect Darts" button
    if(mouseX >= m_collectBtnX && mouseX <= m_collectBtnX + m_collectBtnWidth &&
       mouseY >= m_collectBtnY && mouseY <= m_collectBtnY + m_collectBtnHeight)
    {
        onCollectDartsClicked();
        return;
    }

    // Map click to polar coordinates
    float angle = 0.0f;
    float normRadius = 0.0f;
    if(!mapClickToPolar(mouseX, mouseY, angle, normRadius))
    {
        // Off-board click, ignore
        return;
    }

    // Board is no longer clear
    m_boardClear = false;

    // Fire immediate dart-landed callback
    if(m_onDartLanded)
    {
        m_onDartLanded();
    }

    // Determine segment for visual highlighting
    auto segment = polarToSegment(angle, normRadius);
    DartSegment seg = segment.value_or(DartSegment::OUTER_BULL); // Shouldn't happen since we checked bounds

    if(segment.has_value())
    {
        m_board.highlightSegment(seg);
    }

    // Queue the pending throw
    PendingThrow pending;
    pending.angle            = angle;
    pending.normalizedRadius = normRadius;
    pending.delayRemaining   = m_positionDelay;
    pending.clickX           = mouseX;
    pending.clickY           = mouseY;
    pending.segment          = seg;
    m_pendingThrows.push_back(pending);

    LOG_INFO(VISION_LOG_ID, "Dart clicked at ({}, {}), angle={:.1f}, radius={:.3f}",
             mouseX, mouseY, angle, normRadius);
}


bool SimVisionSource::mapClickToPolar(float mouseX, float mouseY, float& outAngle, float& outNormRadius) const
{
    float dx = mouseX - m_boardCenterX;
    float dy = mouseY - m_boardCenterY;

    float dist = std::sqrt(dx * dx + dy * dy);
    float scaledRadius = BASE_RADIUS * m_boardScale;
    outNormRadius = dist / scaledRadius;

    if(outNormRadius > RADIUS_DOUBLE_OUTER)
    {
        return false; // Off-board
    }

    outAngle = static_cast<float>(std::atan2(dy, dx) * 180.0 / M_PI);
    return true;
}


void SimVisionSource::onCollectDartsClicked()
{
    m_board.unhighlightAll();
    m_markers.clear();
    m_pendingThrows.clear();
    m_boardClear = true;

    LOG_INFO(VISION_LOG_ID, "Darts collected");
}


bool SimVisionSource::isBoardClear() const
{
    return m_boardClear;
}


void SimVisionSource::render()
{
    // Guard against closed window
    if(m_frameId == INVALID_FRAME_ID)
    {
        return;
    }

    // Clear background
    renderQueueClearFrame(m_frameId, 50, 50, 50);

    // Draw the dart board
    m_board.enqueueRender(m_frameId);

    // Draw dart markers
    for(const auto& marker : m_markers)
    {
        auto circle = std::make_shared<RenderShape>();
        circle->m_type  = ShapeType::Circle;
        circle->m_color = {180, 50, 220};
        circle->m_x     = marker.clickX;
        circle->m_y     = marker.clickY;
        circle->m_z     = MARKER_Z;
        circle->m_width = MARKER_DIAMETER;
        renderQueueAdd(m_frameId, circle);
    }

    // Draw "Collect Darts" button background
    auto btnBg = std::make_shared<RenderShape>();
    btnBg->m_type   = ShapeType::Box;
    btnBg->m_color  = {0, 150, 0};
    btnBg->m_x      = m_collectBtnX;
    btnBg->m_y      = m_collectBtnY;
    btnBg->m_z      = BTN_Z;
    btnBg->m_width  = m_collectBtnWidth;
    btnBg->m_height = m_collectBtnHeight;
    renderQueueAdd(m_frameId, btnBg);

    // Draw "Collect Darts" button text
    auto btnText = std::make_shared<RenderText>();
    btnText->m_text     = "Collect Darts";
    btnText->m_color    = {255, 255, 255};
    btnText->m_fontId   = m_fontId;
    btnText->m_rotation = 0.0f;
    btnText->m_scaleX   = 1.0f;
    btnText->m_scaleY   = 1.0f;
    btnText->m_x        = m_collectBtnX + 45.0f;
    btnText->m_y        = m_collectBtnY + 10.0f;
    btnText->m_z        = BTN_Z + 1;
    renderQueueAdd(m_frameId, btnText);

    // Flush and present
    renderQueueDrawFlush(m_frameId);
    presentFrame(m_frameId);
}
