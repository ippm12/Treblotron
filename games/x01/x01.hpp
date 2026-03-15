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

/** In/Out rule variants. */
enum class X01InOutRule : uint8_t
{
    Any,     // No restriction
    Double,  // Must hit a double (or inner bull)
    Master   // Must hit a double, triple, or inner bull
};

class X01Game : public Game
{
    public:
        explicit X01Game(X01Variant variant = X01Variant::V501,
                        X01InOutRule outRule = X01InOutRule::Double,
                        X01InOutRule inRule  = X01InOutRule::Any);
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

        X01Variant   m_variant;
        X01InOutRule m_outRule;
        X01InOutRule m_inRule;
        FontID       m_fontId;
        FontID       m_largeFontId;
        DartBoard    m_board;

        // Per-player scores indexed by player index
        std::vector<uint16_t> m_playerScores;

        // Per-player in-rule tracking (true = player has satisfied the in rule)
        std::vector<bool> m_playerStarted;

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

        // Bust display
        bool        m_showBust  = false;
        float       m_bustTimer = 0.0f;

        // Game over state
        bool        m_gameOver       = false;
        uint8_t     m_winnerIndex    = 0;
        uint8_t     m_gameOverCursor = 0; // 0 = Restart, 1 = Main Menu
};

#endif // X01_HPP
