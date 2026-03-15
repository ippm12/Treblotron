/**
 * x01.hpp
 *
 * X01 dart game (301, 501, 701, etc.). Players take turns throwing
 * three darts per round, counting down from the starting score to
 * exactly zero.
 */

#ifndef X01_HPP
#define X01_HPP

#include "game_lib/game.hpp"
#include "game_lib/entities/dart_board.hpp"
#include <string>
#include <vector>

/** Starting score variants for X01 games. */
enum class X01Variant : uint16_t
{
    V301 = 301,
    V501 = 501,
    V701 = 701,
    V1001 = 1001
};

class X01Game : public Game
{
    public:
        explicit X01Game(X01Variant variant = X01Variant::V501);
        ~X01Game() override = default;

        Status init(FrameID frameId) override;
        void update(float deltaTime) override;
        void render() override;
        void shutdown() override;
        GameBarInfo getBarInfo() const override;
        uint8_t getMaxPlayers() const override;

        // Input callbacks from GameManager
        void onKeyDown(uint32_t keycode) override;
        void onGamepadButton(uint8_t button, bool pressed) override;

    private:
        void renderRightScoreboard();
        void renderLeftPlayerDetail();
        void renderGameOver();

        X01Variant  m_variant;
        FontID      m_fontId;
        FontID      m_largeFontId;
        DartBoard   m_board;

        // Per-player scores indexed by player index
        std::vector<uint16_t> m_playerScores;

        // Turn progression tracking
        uint16_t              m_turnStartScore = 0;
        std::vector<uint16_t> m_turnScoreProgression;

        uint8_t     m_currentPlayerIndex = 0;
        uint8_t     m_throwsRemaining   = 3;
        bool        m_waitingForCollect = false;
        std::string m_statusText        = "Waiting for Throw";

        // Blink state for highlighted segments
        std::vector<DartSegment> m_hitSegments;
        float       m_blinkTimer   = 0.0f;
        bool        m_blinkOn      = true;

        // Game over state
        bool        m_gameOver       = false;
        uint8_t     m_winnerIndex    = 0;
        uint8_t     m_gameOverCursor = 0; // 0 = Restart, 1 = Main Menu
};

#endif // X01_HPP
