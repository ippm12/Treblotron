/**
 * dartfleet.hpp
 *
 * Dartfleet — naval-combat dart game. Two teams secretly place a fleet
 * of three ships (sizes 2, 3, 4) on top of a dartboard, then take turns
 * throwing one dart per turn at the opposing fleet. Cells reveal as
 * misses (purple X) or hits (orange dot); ships only fully reveal when
 * sunk. First team to sink the opposing fleet wins.
 */

#ifndef DARTFLEET_HPP
#define DARTFLEET_HPP

#include "game_lib/game.hpp"
#include "game_lib/game_helpers.hpp"
#include "game_lib/entities/dart_board.hpp"
#include "game_lib/input_hints.hpp"
#include <array>
#include <cstdint>
#include <string>
#include <vector>


/** Per-team cell size — controls difficulty (bigger cells = easier to hit). */
enum class FleetSize : uint8_t
{
    Small   = 0,
    Default = 1,
    Large   = 2
};


/** Phases of a Dartfleet match. */
enum class DartfleetPhase : uint8_t
{
    ConfigError,       // Player/team count is wrong — show message and exit to menu
    NoPeekIntro,       // Pre-game blackout: "[Team 2], no peeking!" while Team 1 places
    PlacementTeam0,    // Team 0 placing ships
    NoPeekMidway,      // Blackout between teams: "[Team 1], no peeking!" while Team 2 places
    PlacementTeam1,    // Team 1 placing ships
    FirstTeamReveal,   // Brief "Team X fires first!" banner
    PlayerTurn,        // Active team throws one dart (waiting-for-collect tracked separately)
    GameOver
};


/** A single square hitbox in ship-local coordinates. */
struct DartfleetCell
{
    float localOffsetX = 0.0f;  // distance along ship's local +X axis
    float localOffsetY = 0.0f;  // always 0 for ships laid out in a line
    bool  hit          = false;
};


/** A ship: rotated bar of N square cells. */
struct DartfleetShip
{
    uint8_t                    size      = 0;     // cell count (2, 3, or 4)
    float                      centerX   = 0.0f;  // screen pixels
    float                      centerY   = 0.0f;
    float                      rotation  = 0.0f;  // radians, CCW from +X
    float                      cellSize  = 36.0f; // pixels per side
    std::vector<DartfleetCell> cells;
    bool                       sunk      = false;
};


/** A purple miss marker on a defender's board. */
struct DartfleetMiss
{
    float x = 0.0f;
    float y = 0.0f;
};


/** All state for one team's home board. */
struct DartfleetTeamState
{
    std::vector<DartfleetShip> ships;        // 3 ships: size 4, 3, 2
    std::vector<DartfleetMiss> misses;       // permanent miss marks
    float                      cellSize = 0; // px per cell side
};


class DartfleetGame : public Game
{
    public:
        DartfleetGame(bool teamsMode, FleetSize team0Size, FleetSize team1Size);
        ~DartfleetGame() override = default;

        Status init(FrameID frameId) override;
        void   update(float deltaTime) override;
        void   render() override;
        void   shutdown() override;
        GameBarInfo getBarInfo() const override;
        uint8_t getMaxPlayers() const override;
        bool    isPauseable() const override;

        void onKeyDown(uint32_t keycode) override;
        void onGamepadButton(uint8_t button, bool pressed) override;

    private:
        // ── Setup ──────────────────────────────────────────────────────
        void buildShipsForTeam(uint8_t teamIdx);

        // ── Placement phase ────────────────────────────────────────────
        void onPlacementMove(float dx, float dy);
        void onPlacementRotate(float dRadians);
        void onPlacementUnconfirm();
        void onPlacementConfirmShip();
        void onPlacementFinishTeam();
        bool isShipPlacementValid(uint8_t teamIdx, uint8_t shipIdx) const;
        bool obbsOverlap(const DartfleetShip& a, const DartfleetShip& b) const;
        void shipCellWorldCenter(const DartfleetShip& s, uint8_t cellIndex,
                                 float& outX, float& outY) const;

        // ── Play phase ─────────────────────────────────────────────────
        void processDart(const DartPosition& pos);
        void advanceTurn();
        bool currentDefenderAllSunk() const;

        // ── Coordinate helpers ─────────────────────────────────────────
        float boardCenterX(uint8_t teamIdx) const;  // teamIdx's own board
        void  polarToBoardPixels(uint8_t teamIdx, float angle, float r,
                                 float& outX, float& outY) const;

        // ── Rendering ──────────────────────────────────────────────────
        void renderTopStrip();
        // Renders the overlays for one screen side. During placement the
        // placing team's own ships go on that side. During play the OPPOSING
        // team's ships are tracked here as the attacker's radar — their
        // sunk hulls, hit cells, and the attacker's misses all render here
        // shifted from the defender-side coordinates.
        void renderBoardOverlays(uint8_t boardSideIdx, bool revealUnsunkShips, bool playPhase);
        void renderShipOutline(const DartfleetShip& s, Color edgeColor,
                               uint32_t z, bool filled, float xShift);
        void renderSunkShip(const DartfleetShip& s, float xShift);
        void renderMissX(float x, float y, uint32_t z);
        void renderActiveSideHighlight();
        void renderPlacementHints();
        void renderGameOverHints();
        void renderNoPeekOverlay(uint8_t teamLeavingIdx);
        void renderConfigError();
        void renderFirstTeamRevealBanner();
        void renderShipSunkBanner();
        void renderGameOver();

        // ── Util ───────────────────────────────────────────────────────
        const std::string& teamName(uint8_t teamIdx) const;

        // ── Static config ──────────────────────────────────────────────
        bool      m_teamsMode;
        FleetSize m_teamFleetSize[2];

        // ── Resources ──────────────────────────────────────────────────
        FontID     m_fontId      = INVALID_FONT_ID;
        FontID     m_largeFontId = INVALID_FONT_ID;
        DartBoard  m_boardLeft;
        DartBoard  m_boardRight;
        InputHints m_inputHints;

        // ── Teams ──────────────────────────────────────────────────────
        // Team mode:    teams from players module (must be exactly 2)
        // Non-team:     each "team" is a single player (must be exactly 2 players)
        DartfleetTeamState m_teams[2];
        std::string        m_teamNames[2];
        TeamTurnTracker    m_turnTracker;     // teams mode only

        // ── Phase state ────────────────────────────────────────────────
        DartfleetPhase m_phase             = DartfleetPhase::NoPeekIntro;
        std::string    m_configErrorMsg;
        uint8_t        m_attackingTeam     = 0;  // Active team during PlayerTurn
        uint8_t        m_activeShipIdx     = 0;  // index of ship being placed
        uint8_t        m_lastSunkTeamIdx   = 0;
        uint8_t        m_lastSunkShipIdx   = 0;
        float          m_phaseTimer        = 0.0f;  // FirstTeamReveal countdown
        float          m_shipSunkTimer     = 0.0f;  // counts up while banner shown
        bool           m_waitingForCollect = false;
        bool           m_pulseOn           = true;
        float          m_pulseTimer        = 0.0f;
        std::string    m_statusText        = "Place your fleet";

        // ── Game over ──────────────────────────────────────────────────
        uint8_t m_winnerTeamIdx  = 0;
        uint8_t m_gameOverCursor = 0;
};


#endif // DARTFLEET_HPP
