/**
 * game_helpers.hpp
 *
 * Shared helper functions for dart games. Provides common rendering
 * (game-over overlay, scoreboard panel), input handling, blink logic,
 * and layout constants so individual games don't duplicate them.
 */

#ifndef GAME_HELPERS_HPP
#define GAME_HELPERS_HPP

#include "game_lib/game.hpp"
#include "game_lib/components/render_object.hpp"
#include "frame/frame.hpp"
#include "players/players.hpp"
#include <cstdint>
#include <string>
#include <vector>


// ============================================================================
// Shared layout constants
// ============================================================================

namespace GameLayout
{
    // Standard dart board position
    constexpr float BOARD_CENTER_X = 960.0f;
    constexpr float BOARD_CENTER_Y = 435.0f;
    constexpr float BOARD_SCALE    = 1.8f;

    constexpr float BLINK_PERIOD = 1.2f;

    // Right scoreboard panel
    constexpr float RIGHT_PANEL_X  = 1410.0f;
    constexpr float RIGHT_PANEL_W  = 480.0f;
    constexpr float SCORE_TOP_Y    = 180.0f;
    constexpr float SCORE_ROW_H    = 75.0f;

    // Z-ordering
    constexpr uint32_t SIDEBAR_Z  = 100;
    constexpr uint32_t OVERLAY_Z  = 500;
}


// ============================================================================
// Game-over overlay
// ============================================================================

/**
 * Render the standard game-over overlay: dark background, center panel,
 * winner announcement, and Restart / Main Menu options.
 */
void renderGameOverOverlay(FrameID frameId, FontID largeFontId, FontID fontId,
                           const std::string& winnerName, uint8_t menuCursor);


/** Result of game-over menu input. */
enum class GameOverAction : uint8_t
{
    None,
    Restart,
    MainMenu
};

/** Handle keyboard input for the game-over menu. Updates cursor in-place. */
GameOverAction handleGameOverKey(uint32_t keycode, uint8_t& cursor);

/** Handle gamepad input for the game-over menu. Updates cursor in-place. */
GameOverAction handleGameOverGamepad(uint8_t button, uint8_t& cursor);


// ============================================================================
// Scoreboard panel
// ============================================================================

/** One row in the right-side scoreboard. */
struct ScoreboardEntry
{
    std::string name;
    std::string value;
    Color       valueColor = {255, 255, 255};
    std::string detailText;                      // optional second line below value
    Color       detailColor = {130, 180, 220};   // default light blue
};

/**
 * Render the right-side scoreboard panel with player rows.
 * Highlights the current player with an accent bar.
 */
void renderScoreboardPanel(FrameID frameId, FontID fontId,
                           const std::vector<ScoreboardEntry>& entries,
                           uint8_t currentPlayerIndex);


// ============================================================================
// Announcement banner
// ============================================================================

/**
 * Render a centered announcement banner over the board area with a dark
 * backdrop panel. Use for checkout/win/leg indicators while waiting for
 * dart collection. Games provide their own heading and message text.
 */
void renderAnnouncementBanner(FrameID frameId, FontID largeFontId,
                               const std::string& heading,
                               const std::string& message,
                               Color messageColor = {40, 220, 80});


// ============================================================================
// Blink helper
// ============================================================================

/**
 * Update blink timer and toggle state. Returns true when the blink state
 * changed (so the caller can respond by highlighting or unhighlighting).
 */
bool updateBlink(float deltaTime, float& blinkTimer, bool& blinkOn,
                 float blinkPeriod = GameLayout::BLINK_PERIOD);


// ============================================================================
// Hit markers
// ============================================================================

/**
 * Render small circle markers at exact dart hit positions on the board.
 * Converts polar coordinates to screen pixels using board center/scale.
 */
void renderHitMarkers(FrameID frameId,
                      const std::vector<DartPosition>& positions,
                      float boardCenterX, float boardCenterY, float boardScale);


// ============================================================================
// Bar info builder
// ============================================================================

/**
 * Build a GameBarInfo struct from common turn-based game state.
 * Handles the standard conditional logic (game over / collecting / playing).
 */
GameBarInfo makeBarInfo(bool gameOver, bool waitingForCollect,
                        uint8_t playerIndex, uint8_t throwsRemaining,
                        const std::string& statusText);


// ============================================================================
// Team-based player ordering
// ============================================================================

/**
 * Build an interleaved player order that alternates between teams.
 * For example, with Team A (players 0,1) and Team B (players 2,3):
 * result = [0, 2, 1, 3]
 *
 * If no teams exist, returns the default sequential order [0, 1, 2, ...].
 * The returned vector contains player indices (not PlayerIDs).
 */
std::vector<uint8_t> buildInterleavedPlayerOrder();

/**
 * Given the current position in the interleaved order and the order vector,
 * return the next position (wraps around).
 */
uint8_t advanceInterleavedPlayer(uint8_t currentOrderPosition,
                                  const std::vector<uint8_t>& order);


// ============================================================================
// Team turn tracker
// ============================================================================

/**
 * Tracks team-based turn order for games that use teams as scoring entities.
 * Each team takes turns, and within each team the throwing player rotates
 * each time that team's turn comes around.
 */
class TeamTurnTracker
{
public:
    /**
     * Initialize from current team/player data in the players module.
     * Builds internal rosters from getTeamCount()/getPlayerIndicesForTeam().
     */
    void init();

    /** Number of teams. */
    uint8_t teamCount() const;

    /** Current team index (0-based, use as index into per-team game state). */
    uint8_t currentTeam() const;

    /** Player index of the current thrower (for getPlayerByIndex/getPlayerName). */
    uint8_t currentPlayerIndex() const;

    /** Team name for team index t. */
    const std::string& teamName(uint8_t teamIndex) const;

    /** Advance to next team. Rotates the thrower within the next team. */
    void advance();

private:
    struct TeamRoster
    {
        TeamID               teamId;
        std::vector<uint8_t> playerIndices;
        uint8_t              nextThrower = 0;
    };

    std::vector<TeamRoster> m_teams;
    uint8_t m_currentTeam = 0;
};


#endif // GAME_HELPERS_HPP
