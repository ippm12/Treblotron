/**
 * vision_debug.hpp
 *
 * Vision debug screen. Displays synchronized segmentation and tip heatmap views,
 * a dartboard with hit markers, and a text list of tracked darts.
 * Allows force-resetting the vision system's dart state.
 */

#ifndef VISION_DEBUG_HPP
#define VISION_DEBUG_HPP

#include "game_lib/game.hpp"
#include "game_lib/entities/dart_board.hpp"
#include "game_lib/input_hints.hpp"
#include "vision/vision.hpp"
#include "dart/dart_defs.hpp"
#include "debug/scoped_timer.hpp"
#include <cstdint>
#include <vector>

struct SDL_Texture;

class VisionDebugScreen : public Game
{
    public:
        VisionDebugScreen();
        ~VisionDebugScreen() override = default;

        Status init(FrameID frameId) override;
        void update(float deltaTime) override;
        void onDartsCollected() override;
        void render() override;
        void shutdown() override;
        bool isPauseable() const override;

        void onKeyDown(uint32_t keycode) override;
        void onGamepadButton(uint8_t button, bool pressed) override;

    private:
        struct TrackedDart
        {
            DartSegment segment;
            float angle;
            float normalizedRadius;
        };

        void resetDarts();
        void updateComposite();

        FontID       m_titleFontId;
        FontID       m_bodyFontId;
        InputHints   m_inputHints;

        // Disabled in simulation builds — there are no cameras and no
        // heatmap, so the screen just shows a friendly message instead of
        // the empty composite + spinning timings.
        bool         m_simMode = false;

        DartBoard    m_board;
        std::vector<TrackedDart>   m_trackedDarts;
        std::vector<DartPosition>  m_hitPositions;

        SDL_Texture*          m_compositeTexture = nullptr;
        std::shared_ptr<const DartPreview> m_displayedPreview;
        std::chrono::steady_clock::time_point m_displayedHeatmapTime{};
        double                m_rateWindowStart = 0;
        uint64_t              m_initialDetectionCount = 0;
        unsigned              m_uiFrames = 0;
        std::string           m_captureStatus;
        std::string           m_rateLabel = "Measuring update rates...";

        FrameTimings          m_timings;
        double                m_lastLogSec = 0.0;
};

#endif // VISION_DEBUG_HPP
