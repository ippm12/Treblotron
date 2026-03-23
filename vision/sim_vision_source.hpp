/**
 * sim_vision_source.hpp
 *
 * Simulated vision source: opens a second SDL window with a clickable
 * dart board. Clicking the board simulates dart throws, triggering the
 * same callbacks that a real camera vision source would.
 *
 * INTERNAL HEADER — do not include from outside the vision module.
 * Use vision/vision.hpp for the public API.
 */

#ifndef SIM_VISION_SOURCE_HPP
#define SIM_VISION_SOURCE_HPP

#include "vision_source.hpp"
#include "game_lib/entities/dart_board.hpp"
#include "dart/dart_defs.hpp"
#include "flecs.h"
#include <vector>
#include <optional>

class SimVisionSource : public VisionSource
{
    public:
        SimVisionSource();
        ~SimVisionSource() override;

        Status init() override;
        void tick(float deltaTime) override;
        void shutdown() override;
        bool isBoardClear() const override;

        /** Set the delay (in seconds) between dart detection and position callback. */
        void setPositionDelay(float seconds) { m_positionDelay = seconds; }

    private:
        /** Handle a mouse click on the sim window. */
        void onBoardClicked(FrameID frameId, float mouseX, float mouseY, uint8_t button);

        /** Convert pixel coordinates to polar (angle + normalizedRadius). Returns false if off-board. */
        bool mapClickToPolar(float mouseX, float mouseY, float& outAngle, float& outNormRadius) const;

        /** Handle "Collect Darts" button click. */
        void onCollectDartsClicked();

        /** Render the sim window contents. */
        void render();

        /** A dart throw waiting for the position delay to expire. */
        struct PendingThrow
        {
            float angle;
            float normalizedRadius;
            float delayRemaining;
            float clickX;
            float clickY;
            DartSegment segment;   // For visual highlighting
        };

        /** A visual marker for a dart that has landed. */
        struct DartMarker
        {
            float clickX;
            float clickY;
        };

        FrameID      m_frameId      = INVALID_FRAME_ID;
        FontID       m_fontId       = INVALID_FONT_ID;
        flecs::world m_world;
        DartBoard    m_board;

        // Board geometry for click mapping
        float m_boardCenterX = 0.0f;
        float m_boardCenterY = 0.0f;
        float m_boardScale   = 1.0f;

        // Pending throws and visual markers
        std::vector<PendingThrow> m_pendingThrows;
        std::vector<DartMarker>   m_markers;

        // Board state
        bool m_boardClear = true;

        // Configuration
        float m_positionDelay = 0.050f; // 50ms default

        // Collect Darts button geometry
        float m_collectBtnX      = 0.0f;
        float m_collectBtnY      = 0.0f;
        float m_collectBtnWidth  = 0.0f;
        float m_collectBtnHeight = 0.0f;
};

#endif // SIM_VISION_SOURCE_HPP
