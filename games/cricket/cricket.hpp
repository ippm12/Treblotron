/**
 * cricket.hpp
 *
 * Cricket dart game. Players close numbers 15-20 and the bullseye by
 * hitting each three times. Supports standard, cut-throat, and no-score
 * variants, with an optional random numbers mode.
 */

#ifndef CRICKET_HPP
#define CRICKET_HPP

#include "game_lib/game.hpp"
#include "game_lib/game_helpers.hpp"
#include "game_lib/entities/dart_board.hpp"
#include <string>
#include <vector>

/** Scoring variant for Cricket. */
enum class CricketScoring : uint8_t
{
    Standard,  // Points go to the player who closed the number
    CutThroat, // Points go to opponents who haven't closed the number
    NoScore    // No scoring — first to close all numbers wins
};

static constexpr uint8_t CRICKET_NUM_TARGETS = 7; // 6 numbers + bull

/** Per-player state in a Cricket game. */
struct CricketPlayerState
{
    uint8_t  marks[CRICKET_NUM_TARGETS] = {}; // marks per target (0-3+)
    uint16_t score = 0;
};

class CricketGame : public Game
{
    public:
        CricketGame(CricketScoring scoring = CricketScoring::Standard,
                    bool randomNumbers = false,
                    bool teamsMode = false);
        ~CricketGame() override = default;

        Status init(FrameID frameId) override;
        void update(float deltaTime) override;
        void render() override;
        void shutdown() override;
        GameBarInfo getBarInfo() const override;
        uint8_t getMaxPlayers() const override;

        // Input callbacks from GameManager
        void onKeyDown(uint32_t keycode) override;
        void onGamepadButton(uint8_t button, bool pressed) override;
        void onMissedThrow() override;

    private:
        void renderMarksScoreboard();
        void renderPointScores();
        void renderGameOver();

        void updateBoardColors();
        int  findTargetIndex(uint8_t section) const;
        bool isNumberClosedByAll(int targetIdx) const;
        bool anyOpponentClosed(int targetIdx) const;
        bool checkWinCondition(uint8_t playerIdx) const;
        uint16_t getTargetFaceValue(int targetIdx) const;

        CricketScoring  m_scoring;
        bool            m_randomNumbers;
        bool            m_teamsMode;
        TeamTurnTracker m_turnTracker;
        FontID         m_fontId;
        FontID         m_largeFontId;
        DartBoard      m_board;

        // Target numbers: section values (15-20 for standard, 0 = bull)
        uint8_t m_targets[CRICKET_NUM_TARGETS] = {};

        // Tracks which player index first closed each target (-1 = nobody)
        int8_t m_firstToClose[CRICKET_NUM_TARGETS] = {-1, -1, -1, -1, -1, -1, -1};

        // Per-player state
        std::vector<CricketPlayerState> m_players;

        // Turn state
        uint8_t     m_currentPlayerIndex = 0;
        uint8_t     m_throwsRemaining    = 3;
        bool        m_waitingForCollect  = false;
        std::string m_statusText         = "Waiting for Throw";

        // Hit tracking for blink flash
        struct HitSegment
        {
            DartSegment segment;
            bool        wasAlreadyClosed; // target had 3+ marks before this hit
        };
        std::vector<HitSegment> m_hitSegments;
        std::vector<DartPosition> m_hitPositions; // exact hit locations for markers
        float       m_blinkTimer = 0.0f;
        bool        m_blinkOn    = true;

        // Game over state
        bool    m_gameOver       = false;
        uint8_t m_winnerIndex    = 0;
        uint8_t m_gameOverCursor = 0; // 0 = Restart, 1 = Main Menu
};

#endif // CRICKET_HPP
