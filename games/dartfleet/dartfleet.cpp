/**
 * dartfleet.cpp
 *
 * Dartfleet implementation. See dartfleet.hpp for game overview.
 */

#include "dartfleet.hpp"
#include "dart/dart_board_geometry.hpp"
#include "frame/render_queue.hpp"
#include "game_lib/game_manager.hpp"
#include "game_lib/palette.hpp"
#include "game_lib/components/render_shape.hpp"
#include "game_lib/components/render_text.hpp"
#include "game_lib/components/render_image.hpp"
#include "games/main_menu.hpp"
#include "players/players.hpp"
#include "vision/vision.hpp"

#include <SDL3/SDL_keycode.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_scancode.h>
#include <SDL3/SDL_gamepad.h>
#include <SDL3_ttf/SDL_ttf.h>

#include <algorithm>
#include <cmath>
#include <random>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif


// ============================================================================
// Constants
// ============================================================================

static constexpr float FONT_SIZE       = 28.0f;
static constexpr float LARGE_FONT_SIZE = 96.0f;
static constexpr float SHIP_LABEL_SIZE = 36.0f;

static constexpr float BOARDS_SCALE    = 1.4f;
static constexpr float BOARD_LEFT_X    = 520.0f;
static constexpr float BOARD_RIGHT_X   = 1400.0f;
static constexpr float BOARDS_Y        = 480.0f;
static constexpr float BOARD_PIXEL_R   = DartBoardGeometry::BASE_RADIUS * BOARDS_SCALE;

static constexpr float TOP_STRIP_Y     = 30.0f;
static constexpr float HINTS_Y         = 820.0f;

// Cell side length per FleetSize: Small / Default / Large.
static constexpr float CELL_SIZES[3]   = { 42.0f, 54.0f, 66.0f };

// Three ships per team — placed in this order (largest first).
static constexpr uint8_t SHIP_SIZES[3] = { 4, 3, 2 };
static constexpr uint8_t NUM_SHIPS     = 3;

// Continuous movement / rotation while a key or button is held.
static constexpr float MOVE_SPEED    = 200.0f;                                   // px/sec
static constexpr float ROTATE_SPEED  = 120.0f * static_cast<float>(M_PI) / 180.0f; // rad/sec

static constexpr float FIRST_TEAM_REVEAL_SECS = 2.5f;
static constexpr float SHIP_SUNK_BANNER_SECS  = 2.5f;
static constexpr float PULSE_PERIOD           = 1.0f;

// Z-order layers (above board labels at ~7, below overlay banners at 500)
static constexpr uint32_t Z_SHIP_HULL    = 50;
static constexpr uint32_t Z_HIT_CIRCLE   = 51;
static constexpr uint32_t Z_MISS_X       = 52;
static constexpr uint32_t Z_SHIP_LABEL   = 53;
static constexpr uint32_t Z_PLACEMENT    = 60;
static constexpr uint32_t Z_TOP_STRIP    = 90;
static constexpr uint32_t Z_HINTS        = 90;
static constexpr uint32_t Z_NO_PEEK      = 1000;

// Colors
static constexpr Color COLOR_TEAM0       = Palette::TEAM0;
static constexpr Color COLOR_TEAM1       = Palette::TEAM1;
static constexpr Color COLOR_HIT_ORANGE  = Palette::WARNING;
static constexpr Color COLOR_MISS_PURPLE = Palette::MARKER;
static constexpr Color COLOR_SHIP_HULL   = {  60,  60,  70 };
static constexpr Color COLOR_VALID       = Palette::CONFIRM;
static constexpr Color COLOR_INVALID     = Palette::INVALID;
static constexpr Color COLOR_DIM         = Palette::TEXT_DIM;
static constexpr Color COLOR_WHITE       = Palette::TEXT;
static constexpr Color COLOR_HEADER_DIM  = Palette::TEXT_DIM;


// ============================================================================
// Constructor
// ============================================================================

DartfleetGame::DartfleetGame(bool teamsMode, FleetSize team0Size, FleetSize team1Size,
                             VolleySize volleySize)
    : Game("Dartfleet")
    , m_teamsMode(teamsMode)
    , m_volleySize(volleySize)
    , m_boardLeft()
    , m_boardRight()
{
    m_teamFleetSize[0] = team0Size;
    m_teamFleetSize[1] = team1Size;
}


// ============================================================================
// Init / shutdown
// ============================================================================

Status DartfleetGame::init(FrameID frameId)
{
    m_frameId = frameId;

    m_fontId = loadFont("assets/fonts/Roboto-Regular.ttf", FONT_SIZE);
    if(m_fontId == INVALID_FONT_ID) return STATUS_ERROR_GENERIC;

    m_largeFontId = loadFont("assets/fonts/Roboto-Regular.ttf", LARGE_FONT_SIZE);
    if(m_largeFontId == INVALID_FONT_ID) return STATUS_ERROR_GENERIC;

    m_inputHints.init();

    // Two boards side-by-side.
    m_boardLeft  = DartBoard::create(m_world, BOARD_LEFT_X,  BOARDS_Y, BOARDS_SCALE, m_fontId, 0);
    m_boardRight = DartBoard::create(m_world, BOARD_RIGHT_X, BOARDS_Y, BOARDS_SCALE, m_fontId, 0);

    // Resolve team identities — Dartfleet requires exactly two sides.
    // If the configuration is wrong we still load successfully but enter a
    // ConfigError phase that prints the problem and lets the user back out.
    if(m_teamsMode)
    {
        if(getTeamCount() != 2)
        {
            m_configErrorMsg = "Dartfleet (teams) needs exactly 2 teams. Configured: "
                               + std::to_string(static_cast<int>(getTeamCount()));
            m_phase = DartfleetPhase::ConfigError;
            return STATUS_OK;
        }
        m_turnTracker.init();
        m_teamNames[0] = m_turnTracker.teamName(0);
        m_teamNames[1] = m_turnTracker.teamName(1);
    }
    else
    {
        if(getPlayerCount() != 2)
        {
            m_configErrorMsg = "Dartfleet (no teams) needs exactly 2 players. Configured: "
                               + std::to_string(static_cast<int>(getPlayerCount()));
            m_phase = DartfleetPhase::ConfigError;
            return STATUS_OK;
        }
        m_teamNames[0] = getPlayerName(getPlayerByIndex(0));
        m_teamNames[1] = getPlayerName(getPlayerByIndex(1));
    }

    // Initialize per-team state and pre-build ship layouts at default positions.
    for(uint8_t t = 0; t < 2; t++)
    {
        m_teams[t].cellSize = CELL_SIZES[static_cast<size_t>(m_teamFleetSize[t])];
        m_teams[t].misses.clear();
        buildShipsForTeam(t);
    }

    m_phase             = DartfleetPhase::NoPeekIntro;
    m_activeShipIdx     = 0;
    m_statusText        = "Place your fleet";
    m_waitingForCollect = false;
    m_phaseTimer        = 0.0f;
    m_shipSunkTimer     = 0.0f;
    m_pulseOn           = true;
    m_pulseTimer        = 0.0f;

    return STATUS_OK;
}


bool DartfleetGame::isPauseable() const
{
    // Disable the global pause hook for any phase that wants to consume the
    // Start button as input, otherwise the game manager intercepts Start
    // before we ever see it.
    switch(m_phase)
    {
        case DartfleetPhase::ConfigError:
        case DartfleetPhase::NoPeekIntro:
        case DartfleetPhase::NoPeekMidway:
        case DartfleetPhase::FirstTeamReveal:
        case DartfleetPhase::GameOver:
            return false;
        default:
            return true;
    }
}


void DartfleetGame::shutdown()
{
    m_inputHints.shutdown();

    if(m_largeFontId != INVALID_FONT_ID) { unloadFont(m_largeFontId); m_largeFontId = INVALID_FONT_ID; }
    if(m_fontId      != INVALID_FONT_ID) { unloadFont(m_fontId);      m_fontId      = INVALID_FONT_ID; }
}


void DartfleetGame::buildShipsForTeam(uint8_t teamIdx)
{
    DartfleetTeamState& st = m_teams[teamIdx];
    st.ships.clear();
    st.ships.reserve(NUM_SHIPS);

    float boardCx = boardCenterX(teamIdx);
    float cs = st.cellSize;

    // Stack the three default ship positions vertically near the center
    // of this team's own board, all axis-aligned to start.
    float startY = BOARDS_Y - cs * 3.0f;
    for(uint8_t i = 0; i < NUM_SHIPS; i++)
    {
        DartfleetShip ship;
        ship.size     = SHIP_SIZES[i];
        ship.cellSize = cs;
        ship.centerX  = boardCx;
        ship.centerY  = startY + i * (cs + 4.0f);
        ship.rotation = 0.0f;
        ship.cells.clear();
        ship.cells.resize(ship.size);
        for(uint8_t c = 0; c < ship.size; c++)
        {
            ship.cells[c].localOffsetX = (static_cast<float>(c) - (static_cast<float>(ship.size) - 1.0f) * 0.5f) * cs;
            ship.cells[c].localOffsetY = 0.0f;
            ship.cells[c].hit          = false;
        }
        ship.sunk = false;
        st.ships.push_back(ship);
    }
}


// ============================================================================
// Bar info / max players
// ============================================================================

GameBarInfo DartfleetGame::getBarInfo() const
{
    if(m_phase == DartfleetPhase::PlayerTurn)
    {
        uint8_t throwerIdx;
        if(m_teamsMode)
        {
            throwerIdx = m_turnTracker.currentPlayerIndex();
        }
        else
        {
            throwerIdx = m_attackingTeam;  // 0 or 1
        }
        return makeBarInfo(false, m_waitingForCollect, throwerIdx,
                           m_throwsRemainingInVolley, m_statusText);
    }

    if(m_phase == DartfleetPhase::GameOver)
    {
        return makeBarInfo(true, false, 0, 0, "");
    }

    // Placement / no-peek / reveal — show a generic blank bar.
    GameBarInfo info;
    info.state = GameState::Blank;
    return info;
}


uint8_t DartfleetGame::getMaxPlayers() const
{
    return 6;
}


// ============================================================================
// Update — phase dispatch
// ============================================================================

void DartfleetGame::update(float deltaTime)
{
    // The game manager clamps deltaTime to 0.25s, but a quarter-second
    // spike on the first frame after a phase transition produces a very
    // visible jump on continuous-action inputs (e.g. a 30° rotation in
    // one tick). Tighten the cap for our own update loop.
    if(deltaTime > 0.05f) deltaTime = 0.05f;

    // Idle pulse for the active ship outline / banners.
    m_pulseTimer += deltaTime;
    if(m_pulseTimer >= PULSE_PERIOD * 0.5f)
    {
        m_pulseTimer = 0.0f;
        m_pulseOn = !m_pulseOn;
    }

    // Ship-sunk banner timer (independent of phase — fires within PlayerTurn).
    if(m_shipSunkTimer > 0.0f)
    {
        m_shipSunkTimer -= deltaTime;
        if(m_shipSunkTimer < 0.0f) m_shipSunkTimer = 0.0f;
    }

    switch(m_phase)
    {
        case DartfleetPhase::PlacementTeam0:
        case DartfleetPhase::PlacementTeam1:
        {
            // Drain (and ignore) any spurious dart events during placement.
            (void)consumeDartLandedCount();
            { DartPosition d; while(popDartPosition(d)) {} }

            // Direct-poll inputs every frame. We deliberately bypass the
            // synthetic D-pad-from-stick events: those are edge-triggered
            // and any missed transition leaves a button "stuck pressed".
            const bool* kb = SDL_GetKeyboardState(nullptr);

            float stickX = getGamepadAxis(SDL_GAMEPAD_AXIS_LEFTX);
            float stickY = getGamepadAxis(SDL_GAMEPAD_AXIS_LEFTY);
            constexpr float STICK_THRESH = 0.5f;

            float dx = 0.0f, dy = 0.0f;
            if(kb && kb[SDL_SCANCODE_A])                                 dx -= 1.0f;
            if(kb && kb[SDL_SCANCODE_D])                                 dx += 1.0f;
            if(kb && kb[SDL_SCANCODE_W])                                 dy -= 1.0f;
            if(kb && kb[SDL_SCANCODE_S])                                 dy += 1.0f;
            if(stickX < -STICK_THRESH ||
               isGamepadButtonHeld(SDL_GAMEPAD_BUTTON_DPAD_LEFT))        dx -= 1.0f;
            if(stickX >  STICK_THRESH ||
               isGamepadButtonHeld(SDL_GAMEPAD_BUTTON_DPAD_RIGHT))       dx += 1.0f;
            if(stickY < -STICK_THRESH ||
               isGamepadButtonHeld(SDL_GAMEPAD_BUTTON_DPAD_UP))          dy -= 1.0f;
            if(stickY >  STICK_THRESH ||
               isGamepadButtonHeld(SDL_GAMEPAD_BUTTON_DPAD_DOWN))        dy += 1.0f;

            // Clamp to ±1 since stick + d-pad / WASD can both fire the same direction.
            if(dx >  1.0f) dx =  1.0f;
            if(dx < -1.0f) dx = -1.0f;
            if(dy >  1.0f) dy =  1.0f;
            if(dy < -1.0f) dy = -1.0f;
            if(dx != 0.0f || dy != 0.0f)
            {
                float len = std::sqrt(dx * dx + dy * dy);
                onPlacementMove((dx / len) * MOVE_SPEED * deltaTime,
                                (dy / len) * MOVE_SPEED * deltaTime);
            }

            float dr = 0.0f;
            if(kb && kb[SDL_SCANCODE_Q])                                 dr -= 1.0f;
            if(kb && kb[SDL_SCANCODE_E])                                 dr += 1.0f;
            if(isGamepadButtonHeld(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER))    dr -= 1.0f;
            if(isGamepadButtonHeld(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER))   dr += 1.0f;
            if(dr >  1.0f) dr =  1.0f;
            if(dr < -1.0f) dr = -1.0f;
            if(dr != 0.0f)
            {
                onPlacementRotate(dr * ROTATE_SPEED * deltaTime);
            }
            break;
        }

        case DartfleetPhase::ConfigError:
        case DartfleetPhase::NoPeekIntro:
        case DartfleetPhase::NoPeekMidway:
            (void)consumeDartLandedCount();
            { DartPosition d; while(popDartPosition(d)) {} }
            break;

        case DartfleetPhase::FirstTeamReveal:
            (void)consumeDartLandedCount();
            { DartPosition d; while(popDartPosition(d)) {} }
            m_phaseTimer += deltaTime;
            if(m_phaseTimer >= FIRST_TEAM_REVEAL_SECS)
            {
                m_phase                   = DartfleetPhase::PlayerTurn;
                m_phaseTimer              = 0.0f;
                m_throwsRemainingInVolley = volleySizeFor(m_attackingTeam);
                m_statusText              = "Waiting for Throw";
            }
            break;

        case DartfleetPhase::PlayerTurn:
        {
            uint32_t landed = consumeDartLandedCount();
            DartPosition pos;
            bool hasPos = popDartPosition(pos);

            if(landed > 0 && !hasPos && !m_waitingForCollect)
            {
                m_statusText = "Hold";
            }

            while(hasPos)
            {
                // processDart can switch to GameOver mid-volley if it sinks
                // the last opposing ship. Bail out so we don't process the
                // tail of the volley after the game is decided.
                if(m_phase != DartfleetPhase::PlayerTurn) break;

                if(m_waitingForCollect)
                {
                    // Volley finished; ignore any extra darts until collection.
                    hasPos = popDartPosition(pos);
                    continue;
                }
                processDart(pos);
                hasPos = popDartPosition(pos);
            }

            // Volley ended cleanly (no game-over) — wait for darts to be
            // collected, then hand off to the other team.
            if(m_phase == DartfleetPhase::PlayerTurn
            && m_waitingForCollect && isBoardClear())
            {
                advanceTurn();
            }
            break;
        }

        case DartfleetPhase::GameOver:
            (void)consumeDartLandedCount();
            { DartPosition d; while(popDartPosition(d)) {} }
            break;
    }
}


// ============================================================================
// Render — phase dispatch
// ============================================================================

void DartfleetGame::render()
{
    FrameID fid = getFrameId();

    if(m_phase == DartfleetPhase::ConfigError)
    {
        renderConfigError();
        return;
    }

    // Placement: only the placing team's own board is rendered.
    if(m_phase == DartfleetPhase::PlacementTeam0 || m_phase == DartfleetPhase::PlacementTeam1)
    {
        uint8_t placingTeam = (m_phase == DartfleetPhase::PlacementTeam0) ? 0 : 1;
        if(placingTeam == 0) m_boardLeft.enqueueRender(fid);
        else                 m_boardRight.enqueueRender(fid);

        renderTopStrip();
        renderBoardOverlays(placingTeam, /*revealUnsunkShips*/ true, /*playPhase*/ false);
        renderPlacementHints();
        return;
    }

    if(m_phase == DartfleetPhase::NoPeekIntro)
    {
        // Team 1 is about to place — Team 2 should look away.
        renderNoPeekOverlay(/*teamLeavingIdx*/ 1);
        return;
    }
    if(m_phase == DartfleetPhase::NoPeekMidway)
    {
        // Team 2 is about to place — Team 1 should look away.
        renderNoPeekOverlay(/*teamLeavingIdx*/ 0);
        return;
    }

    // From FirstTeamReveal onward, both boards are visible (radar mode).
    m_boardLeft.enqueueRender(fid);
    m_boardRight.enqueueRender(fid);

    renderTopStrip();
    renderBoardOverlays(0, /*revealUnsunkShips*/ false, /*playPhase*/ true);
    renderBoardOverlays(1, /*revealUnsunkShips*/ false, /*playPhase*/ true);

    if(m_phase == DartfleetPhase::PlayerTurn)
    {
        renderActiveSideHighlight();
    }

    if(m_phase == DartfleetPhase::FirstTeamReveal)
    {
        renderFirstTeamRevealBanner();
        return;
    }

    if(m_shipSunkTimer > 0.0f)
    {
        renderShipSunkBanner();
    }

    if(m_phase == DartfleetPhase::GameOver)
    {
        renderGameOver();
        renderGameOverHints();
    }
}


// ============================================================================
// Input dispatch
// ============================================================================

void DartfleetGame::onKeyDown(uint32_t keycode)
{
    switch(m_phase)
    {
        case DartfleetPhase::PlacementTeam0:
        case DartfleetPhase::PlacementTeam1:
            // Movement & rotation are polled continuously in update() —
            // here we only handle one-shot actions.
            switch(keycode)
            {
                case SDLK_RETURN:    onPlacementConfirmShip(); break;
                case SDLK_BACKSPACE: onPlacementUnconfirm(); break;
                case SDLK_SPACE:     onPlacementFinishTeam(); break;
                default: break;
            }
            break;

        case DartfleetPhase::NoPeekIntro:
            (void)keycode;  // any key advances
            m_phase         = DartfleetPhase::PlacementTeam0;
            m_activeShipIdx = 0;
            break;

        case DartfleetPhase::NoPeekMidway:
            (void)keycode;  // any key advances
            m_phase         = DartfleetPhase::PlacementTeam1;
            m_activeShipIdx = 0;
            break;

        case DartfleetPhase::ConfigError:
            (void)keycode;  // any key returns to main menu
            loadGame(std::make_shared<MainMenu>());
            break;

        case DartfleetPhase::FirstTeamReveal:
        case DartfleetPhase::PlayerTurn:
            // No keyboard interaction during play — darts are the input.
            break;

        case DartfleetPhase::GameOver:
        {
            auto action = handleGameOverKey(keycode, m_gameOverCursor);
            if(action == GameOverAction::Restart)       restartCurrentGame();
            else if(action == GameOverAction::MainMenu) loadGame(std::make_shared<MainMenu>());
            break;
        }
    }
}


void DartfleetGame::onGamepadButton(uint8_t button, bool pressed)
{
    if(!pressed) return;

    switch(m_phase)
    {
        case DartfleetPhase::PlacementTeam0:
        case DartfleetPhase::PlacementTeam1:
            switch(button)
            {
                case SDL_GAMEPAD_BUTTON_SOUTH: onPlacementConfirmShip(); break;     // A
                case SDL_GAMEPAD_BUTTON_EAST:  onPlacementUnconfirm();   break;     // B
                case SDL_GAMEPAD_BUTTON_NORTH: onPlacementFinishTeam();  break;     // Y
                default: break;
            }
            break;

        case DartfleetPhase::NoPeekIntro:
            (void)button;  // any button advances
            m_phase         = DartfleetPhase::PlacementTeam0;
            m_activeShipIdx = 0;
            break;

        case DartfleetPhase::NoPeekMidway:
            (void)button;  // any button advances
            m_phase         = DartfleetPhase::PlacementTeam1;
            m_activeShipIdx = 0;
            break;

        case DartfleetPhase::ConfigError:
            (void)button;  // any button returns to main menu
            loadGame(std::make_shared<MainMenu>());
            break;

        case DartfleetPhase::FirstTeamReveal:
        case DartfleetPhase::PlayerTurn:
            break;

        case DartfleetPhase::GameOver:
        {
            auto action = handleGameOverGamepad(button, m_gameOverCursor);
            if(action == GameOverAction::Restart)       restartCurrentGame();
            else if(action == GameOverAction::MainMenu) loadGame(std::make_shared<MainMenu>());
            break;
        }
    }
}


// ============================================================================
// Placement — input handlers
// ============================================================================

static uint8_t placingTeamFromPhase(DartfleetPhase p)
{
    return (p == DartfleetPhase::PlacementTeam1) ? 1 : 0;
}


void DartfleetGame::onPlacementMove(float dx, float dy)
{
    uint8_t teamIdx = placingTeamFromPhase(m_phase);
    if(m_activeShipIdx >= NUM_SHIPS) return;
    auto& ship = m_teams[teamIdx].ships[m_activeShipIdx];
    ship.centerX += dx;
    ship.centerY += dy;
}


void DartfleetGame::onPlacementRotate(float dRadians)
{
    uint8_t teamIdx = placingTeamFromPhase(m_phase);
    if(m_activeShipIdx >= NUM_SHIPS) return;
    auto& ship = m_teams[teamIdx].ships[m_activeShipIdx];
    ship.rotation += dRadians;
    // Wrap rotation to [-π, π]
    while(ship.rotation >  static_cast<float>(M_PI)) ship.rotation -= 2.0f * static_cast<float>(M_PI);
    while(ship.rotation < -static_cast<float>(M_PI)) ship.rotation += 2.0f * static_cast<float>(M_PI);
}


void DartfleetGame::onPlacementUnconfirm()
{
    if(m_activeShipIdx > 0)
    {
        m_activeShipIdx--;
    }
}


void DartfleetGame::onPlacementConfirmShip()
{
    uint8_t teamIdx = placingTeamFromPhase(m_phase);
    if(m_activeShipIdx >= NUM_SHIPS) return;
    if(!isShipPlacementValid(teamIdx, m_activeShipIdx)) return;
    m_activeShipIdx++;
}


void DartfleetGame::onPlacementFinishTeam()
{
    uint8_t teamIdx = placingTeamFromPhase(m_phase);

    // All three ships must be confirmed and individually valid.
    if(m_activeShipIdx < NUM_SHIPS) return;
    for(uint8_t i = 0; i < NUM_SHIPS; i++)
    {
        if(!isShipPlacementValid(teamIdx, i)) return;
    }

    if(m_phase == DartfleetPhase::PlacementTeam0)
    {
        m_phase         = DartfleetPhase::NoPeekMidway;
        m_activeShipIdx = 0;
    }
    else
    {
        // Both teams done — pick attacker randomly and reveal.
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<int> dist(0, 1);
        m_attackingTeam = static_cast<uint8_t>(dist(gen));

        if(m_teamsMode)
        {
            // The TeamTurnTracker walks teams in registration order starting
            // at team 0. Advance once if attacker should be team 1.
            if(m_attackingTeam == 1) m_turnTracker.advance();
        }

        m_phase      = DartfleetPhase::FirstTeamReveal;
        m_phaseTimer = 0.0f;
    }
}


// ============================================================================
// Placement validation
// ============================================================================

bool DartfleetGame::isShipPlacementValid(uint8_t teamIdx, uint8_t shipIdx) const
{
    const auto& st   = m_teams[teamIdx];
    const auto& ship = st.ships[shipIdx];

    // Every corner of every cell must lie inside the double ring — the
    // user explicitly wants no part of any ship sticking past the playing
    // surface, including the diagonal corners of square cells.
    float boardCx  = boardCenterX(teamIdx);
    float boardCy  = BOARDS_Y;
    float maxR2    = BOARD_PIXEL_R * BOARD_PIXEL_R;
    float c        = std::cos(ship.rotation);
    float si       = std::sin(ship.rotation);
    float halfCell = ship.cellSize * 0.5f;

    for(uint8_t cellIdx = 0; cellIdx < ship.cells.size(); cellIdx++)
    {
        float lcx = ship.cells[cellIdx].localOffsetX;
        float lcy = ship.cells[cellIdx].localOffsetY;
        const float lx[4] = { lcx - halfCell, lcx + halfCell, lcx + halfCell, lcx - halfCell };
        const float ly[4] = { lcy - halfCell, lcy - halfCell, lcy + halfCell, lcy + halfCell };
        for(int k = 0; k < 4; k++)
        {
            float wx = ship.centerX + lx[k] * c - ly[k] * si;
            float wy = ship.centerY + lx[k] * si + ly[k] * c;
            float dx = wx - boardCx;
            float dy = wy - boardCy;
            if(dx * dx + dy * dy > maxR2) return false;
        }
    }

    // Must not overlap any other ship of the same team that comes before
    // it in the confirmed list (idx < shipIdx).
    for(uint8_t j = 0; j < shipIdx; j++)
    {
        if(obbsOverlap(ship, st.ships[j])) return false;
    }

    return true;
}


// OBB-vs-OBB SAT for two ships. Each ship is a rotated rectangle of width
// (size * cellSize) along its local +X and height (cellSize) along +Y.
bool DartfleetGame::obbsOverlap(const DartfleetShip& a, const DartfleetShip& b) const
{
    auto axes = [](const DartfleetShip& s, float& ax, float& ay, float& bx, float& by) {
        float c = std::cos(s.rotation), si = std::sin(s.rotation);
        ax = c;  ay = si;     // ship +X axis
        bx = -si; by = c;     // ship +Y axis
    };

    auto corners = [](const DartfleetShip& s, float out[4][2]) {
        float halfW = s.size * s.cellSize * 0.5f;
        float halfH = s.cellSize * 0.5f;
        float c = std::cos(s.rotation), si = std::sin(s.rotation);
        const float lx[4] = { -halfW, +halfW, +halfW, -halfW };
        const float ly[4] = { -halfH, -halfH, +halfH, +halfH };
        for(int i = 0; i < 4; i++)
        {
            out[i][0] = s.centerX + lx[i] * c - ly[i] * si;
            out[i][1] = s.centerY + lx[i] * si + ly[i] * c;
        }
    };

    float aCorners[4][2], bCorners[4][2];
    corners(a, aCorners);
    corners(b, bCorners);

    auto projectInterval = [](float corners4[4][2], float ax, float ay,
                              float& outMin, float& outMax) {
        outMin = outMax = corners4[0][0] * ax + corners4[0][1] * ay;
        for(int i = 1; i < 4; i++)
        {
            float p = corners4[i][0] * ax + corners4[i][1] * ay;
            outMin = std::min(outMin, p);
            outMax = std::max(outMax, p);
        }
    };

    float ax[2][2]; // [axisIdx][component]
    axes(a, ax[0][0], ax[0][1], ax[1][0], ax[1][1]);
    float bx[2][2];
    axes(b, bx[0][0], bx[0][1], bx[1][0], bx[1][1]);

    const float (*allAxes[4])[2] = { &ax[0], &ax[1], &bx[0], &bx[1] };
    for(int k = 0; k < 4; k++)
    {
        float axx = (*allAxes[k])[0];
        float axy = (*allAxes[k])[1];
        float aMin, aMax, bMin, bMax;
        projectInterval(aCorners, axx, axy, aMin, aMax);
        projectInterval(bCorners, axx, axy, bMin, bMax);
        if(aMax < bMin || bMax < aMin) return false;  // separating axis found
    }
    return true;
}


void DartfleetGame::shipCellWorldCenter(const DartfleetShip& s, uint8_t cellIndex,
                                        float& outX, float& outY) const
{
    float lx = s.cells[cellIndex].localOffsetX;
    float ly = s.cells[cellIndex].localOffsetY;
    float c  = std::cos(s.rotation);
    float si = std::sin(s.rotation);
    outX = s.centerX + lx * c - ly * si;
    outY = s.centerY + lx * si + ly * c;
}


// ============================================================================
// Play phase
// ============================================================================

void DartfleetGame::processDart(const DartPosition& pos)
{
    uint8_t defender = (m_attackingTeam == 0) ? 1 : 0;

    float wx, wy;
    polarToBoardPixels(defender, pos.angle, pos.normalizedRadius, wx, wy);

    bool registered = false;
    auto& dst = m_teams[defender];

    for(uint8_t shipIdx = 0; shipIdx < dst.ships.size(); shipIdx++)
    {
        auto& ship = dst.ships[shipIdx];
        if(ship.sunk) continue;

        // Transform world point into ship-local space.
        float dx = wx - ship.centerX;
        float dy = wy - ship.centerY;
        float c  = std::cos(ship.rotation);
        float si = std::sin(ship.rotation);
        float lx =  dx * c + dy * si;
        float ly = -dx * si + dy * c;

        for(uint8_t cellIdx = 0; cellIdx < ship.cells.size(); cellIdx++)
        {
            auto& cell = ship.cells[cellIdx];
            if(cell.hit) continue;

            float ddx = std::fabs(lx - cell.localOffsetX);
            float ddy = std::fabs(ly - cell.localOffsetY);
            float half = ship.cellSize * 0.5f;
            if(ddx <= half && ddy <= half)
            {
                cell.hit = true;
                registered = true;

                // Check if all cells now hit → ship sunk.
                bool allHit = true;
                for(const auto& cc : ship.cells) if(!cc.hit) { allHit = false; break; }
                if(allHit)
                {
                    ship.sunk = true;
                    m_lastSunkTeamIdx = defender;
                    m_lastSunkShipIdx = shipIdx;
                    m_shipSunkTimer   = SHIP_SUNK_BANNER_SECS;
                }
                break;
            }
        }
        if(registered) break;
    }

    if(!registered)
    {
        DartfleetMiss miss;
        miss.x = wx;
        miss.y = wy;
        dst.misses.push_back(miss);
    }

    // Consume one dart from the active volley.
    if(m_throwsRemainingInVolley > 0) m_throwsRemainingInVolley--;

    // Sinking the defender's last ship ends the game immediately, even if
    // the attacker still has darts left in the volley — those remaining
    // darts are dropped on the floor.
    if(currentDefenderAllSunk())
    {
        m_winnerTeamIdx     = m_attackingTeam;
        m_gameOverCursor    = 0;
        m_phase             = DartfleetPhase::GameOver;
        m_waitingForCollect = false;
        m_statusText        = "";
        return;
    }

    // Volley not done — keep throwing.
    if(m_throwsRemainingInVolley > 0)
    {
        m_statusText = "Waiting for Throw";
        return;
    }

    // Volley finished — wait for the player to collect their darts before
    // the next team takes over.
    m_waitingForCollect = true;
    m_statusText        = "Waiting for Throw";
}


void DartfleetGame::onMissedThrow()
{
    // Mark one dart in the volley as missed: just decrement the counter.
    // No miss-X overlay since we don't know where the dart actually went.
    if(m_phase != DartfleetPhase::PlayerTurn) return;
    if(m_waitingForCollect)                   return;
    if(m_throwsRemainingInVolley == 0)        return;

    m_throwsRemainingInVolley--;
    if(m_throwsRemainingInVolley == 0)
    {
        m_waitingForCollect = true;
    }
    m_statusText = "Waiting for Throw";
}


void DartfleetGame::advanceTurn()
{
    m_waitingForCollect = false;
    m_attackingTeam     = (m_attackingTeam == 0) ? 1 : 0;
    if(m_teamsMode)
    {
        m_turnTracker.advance();
    }
    m_throwsRemainingInVolley = volleySizeFor(m_attackingTeam);
    m_statusText              = "Waiting for Throw";
}


bool DartfleetGame::currentDefenderAllSunk() const
{
    uint8_t defender = (m_attackingTeam == 0) ? 1 : 0;
    for(const auto& ship : m_teams[defender].ships)
    {
        if(!ship.sunk) return false;
    }
    return true;
}


uint8_t DartfleetGame::volleySizeFor(uint8_t attackerIdx) const
{
    switch(m_volleySize)
    {
        case VolleySize::Salvo:
        {
            // Number of the attacker's own ships still afloat. Floor at 1
            // so a defeated team always gets at least one dart per turn —
            // even though that floor case shouldn't actually happen
            // (game ends before any team hits zero ships).
            uint8_t alive = 0;
            for(const auto& s : m_teams[attackerIdx].ships)
            {
                if(!s.sunk) alive++;
            }
            return alive == 0 ? 1 : alive;
        }
        case VolleySize::Three: return 3;
        case VolleySize::Two:   return 2;
        case VolleySize::One:   return 1;
    }
    return 1;
}


// ============================================================================
// Coordinate helpers
// ============================================================================

float DartfleetGame::boardCenterX(uint8_t teamIdx) const
{
    return (teamIdx == 0) ? BOARD_LEFT_X : BOARD_RIGHT_X;
}


void DartfleetGame::polarToBoardPixels(uint8_t teamIdx, float angle, float r,
                                       float& outX, float& outY) const
{
    float radPx = BOARD_PIXEL_R * r;
    float a     = angle * static_cast<float>(M_PI) / 180.0f;
    outX = boardCenterX(teamIdx) + std::cos(a) * radPx;
    outY = BOARDS_Y               + std::sin(a) * radPx;
}


const std::string& DartfleetGame::teamName(uint8_t teamIdx) const
{
    return m_teamNames[teamIdx];
}


// ============================================================================
// Rendering — top strip
// ============================================================================

void DartfleetGame::renderTopStrip()
{
    FrameID fid = getFrameId();

    // Backdrop band
    auto bg = std::make_shared<RenderShape>();
    bg->m_type   = ShapeType::Box;
    bg->m_color  = { 18, 18, 26 };
    bg->m_x      = 0.0f;
    bg->m_y      = 0.0f;
    bg->m_z      = Z_TOP_STRIP;
    bg->m_width  = 1920.0f;
    bg->m_height = 140.0f;
    renderQueueAdd(fid, bg);

    bool showLeft  = true;
    bool showRight = true;
    bool team0Active = false;
    bool team1Active = false;

    if(m_phase == DartfleetPhase::PlacementTeam0) { showRight = false; team0Active = true; }
    if(m_phase == DartfleetPhase::PlacementTeam1) { showLeft  = false; team1Active = true; }
    if(m_phase == DartfleetPhase::PlayerTurn || m_phase == DartfleetPhase::FirstTeamReveal)
    {
        team0Active = (m_attackingTeam == 0);
        team1Active = (m_attackingTeam == 1);
    }

    auto drawName = [&](const std::string& name, float xCenter, Color c, bool active) {
        TTF_Font* font = getFont(m_largeFontId);
        int w = 0, h = 0;
        if(font) TTF_GetStringSize(font, name.c_str(), 0, &w, &h);
        float scale = 0.55f;

        auto txt = std::make_shared<RenderText>();
        txt->m_text     = name;
        txt->m_color    = active ? c : Color{ 110, 110, 120 };
        txt->m_fontId   = m_largeFontId;
        txt->m_rotation = 0.0f;
        txt->m_scaleX   = scale;
        txt->m_scaleY   = scale;
        txt->m_x        = xCenter - static_cast<float>(w) * scale * 0.5f;
        txt->m_y        = TOP_STRIP_Y;
        txt->m_z        = Z_TOP_STRIP + 1;
        renderQueueAdd(fid, txt);
    };

    if(showLeft)  drawName(m_teamNames[0], BOARD_LEFT_X,  COLOR_TEAM0, team0Active);
    if(showRight) drawName(m_teamNames[1], BOARD_RIGHT_X, COLOR_TEAM1, team1Active);

    // Phase / status banner in the middle
    std::string banner;
    Color bannerColor = COLOR_WHITE;
    switch(m_phase)
    {
        case DartfleetPhase::PlacementTeam0:
        case DartfleetPhase::PlacementTeam1:
            banner = "Place your fleet — confirm each ship, then finish to lock in";
            bannerColor = COLOR_DIM;
            break;
        case DartfleetPhase::ConfigError:
        case DartfleetPhase::NoPeekIntro:
        case DartfleetPhase::NoPeekMidway:
            banner = "";
            break;
        case DartfleetPhase::FirstTeamReveal:
            banner = teamName(m_attackingTeam) + " fires first!";
            bannerColor = (m_attackingTeam == 0) ? COLOR_TEAM0 : COLOR_TEAM1;
            break;
        case DartfleetPhase::PlayerTurn:
            banner = teamName(m_attackingTeam) + " — fire!";
            bannerColor = (m_attackingTeam == 0) ? COLOR_TEAM0 : COLOR_TEAM1;
            break;
        case DartfleetPhase::GameOver:
            banner = "";
            break;
    }
    if(!banner.empty())
    {
        TTF_Font* font = getFont(m_fontId);
        int w = 0, h = 0;
        if(font) TTF_GetStringSize(font, banner.c_str(), 0, &w, &h);

        auto txt = std::make_shared<RenderText>();
        txt->m_text     = banner;
        txt->m_color    = bannerColor;
        txt->m_fontId   = m_fontId;
        txt->m_rotation = 0.0f;
        txt->m_scaleX   = 1.0f;
        txt->m_scaleY   = 1.0f;
        txt->m_x        = 960.0f - static_cast<float>(w) * 0.5f;
        txt->m_y        = 90.0f;
        txt->m_z        = Z_TOP_STRIP + 1;
        renderQueueAdd(fid, txt);
    }
}


// ============================================================================
// Rendering — board overlays (ships, hits, misses)
// ============================================================================

void DartfleetGame::renderBoardOverlays(uint8_t boardSideIdx, bool revealUnsunkShips, bool playPhase)
{
    // During play, each screen side is the corresponding team's RADAR — it
    // shows their attacks landing on the *opposing* team's hidden fleet.
    // We translate the defender-side coordinates over to the attacker's
    // board with a horizontal shift.
    uint8_t shipsOwnerIdx;
    float   xShift;
    if(playPhase)
    {
        shipsOwnerIdx = (boardSideIdx == 0) ? 1 : 0;
        xShift        = boardCenterX(boardSideIdx) - boardCenterX(shipsOwnerIdx);
    }
    else
    {
        shipsOwnerIdx = boardSideIdx;
        xShift        = 0.0f;
    }

    const auto& st = m_teams[shipsOwnerIdx];

    // Sunk ship hulls + cell circles (gray base under orange hits).
    for(uint8_t shipIdx = 0; shipIdx < st.ships.size(); shipIdx++)
    {
        const auto& ship = st.ships[shipIdx];
        if(ship.sunk)
        {
            renderSunkShip(ship, xShift);
        }
        else if(revealUnsunkShips)
        {
            // Placement view: draw ship outlines for the placing team.
            bool isActive  = (shipIdx == m_activeShipIdx);
            bool isInvalid = isActive && !isShipPlacementValid(shipsOwnerIdx, shipIdx);
            Color c;
            if(isInvalid)        c = COLOR_INVALID;
            else if(isActive)    c = m_pulseOn ? COLOR_VALID : Color{ 80, 160, 80 };
            else                 c = (shipsOwnerIdx == 0) ? COLOR_TEAM0 : COLOR_TEAM1;
            renderShipOutline(ship, c, Z_PLACEMENT, /*filled*/ isActive, xShift);
        }
    }

    // Orange hit circles for any cell already hit (sunk or not).
    for(const auto& ship : st.ships)
    {
        for(uint8_t c = 0; c < ship.cells.size(); c++)
        {
            if(!ship.cells[c].hit) continue;
            float wx, wy;
            shipCellWorldCenter(ship, c, wx, wy);

            auto circle = std::make_shared<RenderShape>();
            circle->m_type   = ShapeType::Circle;
            circle->m_color  = COLOR_HIT_ORANGE;
            circle->m_x      = wx + xShift;
            circle->m_y      = wy;
            circle->m_z      = Z_HIT_CIRCLE;
            circle->m_width  = ship.cellSize;  // diameter == cell side length
            circle->m_height = ship.cellSize;
            renderQueueAdd(getFrameId(), circle);
        }
    }

    // Purple X for misses (these were thrown by the team attacking shipsOwnerIdx).
    for(const auto& miss : st.misses)
    {
        renderMissX(miss.x + xShift, miss.y, Z_MISS_X);
    }
}


void DartfleetGame::renderShipOutline(const DartfleetShip& s, Color edgeColor,
                                      uint32_t z, bool filled, float xShift)
{
    FrameID fid = getFrameId();

    // Each cell is rendered as two stacked rotated rectangles: an outer one
    // in the edge color, and a slightly smaller inner one in a fill color.
    // The visible difference forms a clean rotated border.
    Color fillColor;
    if(filled)
    {
        fillColor = { static_cast<uint8_t>(edgeColor.r / 3),
                      static_cast<uint8_t>(edgeColor.g / 3),
                      static_cast<uint8_t>(edgeColor.b / 3) };
    }
    else
    {
        fillColor = { static_cast<uint8_t>(edgeColor.r / 5),
                      static_cast<uint8_t>(edgeColor.g / 5),
                      static_cast<uint8_t>(edgeColor.b / 5) };
    }

    constexpr float BORDER_PX = 3.0f;

    for(uint8_t c = 0; c < s.cells.size(); c++)
    {
        float wx, wy;
        shipCellWorldCenter(s, c, wx, wy);
        wx += xShift;

        float outerHalf = s.cellSize * 0.5f;
        auto outer = std::make_shared<RenderShape>();
        outer->m_type     = ShapeType::Box;
        outer->m_color    = edgeColor;
        outer->m_x        = wx - outerHalf;
        outer->m_y        = wy - outerHalf;
        outer->m_width    = s.cellSize;
        outer->m_height   = s.cellSize;
        outer->m_rotation = s.rotation;
        outer->m_z        = z;
        renderQueueAdd(fid, outer);

        float innerSide = s.cellSize - BORDER_PX * 2.0f;
        float innerHalf = innerSide * 0.5f;
        auto inner = std::make_shared<RenderShape>();
        inner->m_type     = ShapeType::Box;
        inner->m_color    = fillColor;
        inner->m_x        = wx - innerHalf;
        inner->m_y        = wy - innerHalf;
        inner->m_width    = innerSide;
        inner->m_height   = innerSide;
        inner->m_rotation = s.rotation;
        inner->m_z        = z + 1;
        renderQueueAdd(fid, inner);
    }
}


void DartfleetGame::renderSunkShip(const DartfleetShip& s, float xShift)
{
    FrameID fid = getFrameId();

    // Single rotated rectangular hull spanning all cells of the ship.
    float hullW = s.size * s.cellSize;
    float hullH = s.cellSize;
    auto hull = std::make_shared<RenderShape>();
    hull->m_type     = ShapeType::Box;
    hull->m_color    = COLOR_SHIP_HULL;
    hull->m_x        = s.centerX + xShift - hullW * 0.5f;
    hull->m_y        = s.centerY - hullH * 0.5f;
    hull->m_width    = hullW;
    hull->m_height   = hullH;
    hull->m_rotation = s.rotation;
    hull->m_z        = Z_SHIP_HULL;
    renderQueueAdd(fid, hull);

    // Class label rotated to ship axis.
    const char* label = (s.size >= 4) ? "Battleship"
                      : (s.size == 3) ? "Submarine"
                      :                 "Patrol";
    TTF_Font* font = getFont(m_fontId);
    int w = 0, h = 0;
    if(font) TTF_GetStringSize(font, label, 0, &w, &h);

    // Pick a scale that keeps the label within the hull's length, capped so
    // it never gets visually larger than half the cell size in height.
    float maxByLength = (w > 0) ? (hullW * 0.85f) / static_cast<float>(w) : 0.7f;
    float maxByHeight = (h > 0) ? (hullH * 0.55f) / static_cast<float>(h) : 0.7f;
    float scale = std::min({ 0.7f, maxByLength, maxByHeight });

    auto txt = std::make_shared<RenderText>();
    txt->m_text     = label;
    txt->m_color    = COLOR_WHITE;
    txt->m_fontId   = m_fontId;
    txt->m_rotation = s.rotation * 180.0f / static_cast<float>(M_PI);
    txt->m_scaleX   = scale;
    txt->m_scaleY   = scale;
    txt->m_x        = s.centerX + xShift - static_cast<float>(w) * 0.5f * scale;
    txt->m_y        = s.centerY - static_cast<float>(h) * 0.5f * scale;
    txt->m_z        = Z_SHIP_LABEL;
    renderQueueAdd(fid, txt);
}


void DartfleetGame::renderActiveSideHighlight()
{
    FrameID fid = getFrameId();
    Color   c   = (m_attackingTeam == 0) ? COLOR_TEAM0 : COLOR_TEAM1;

    constexpr float BAR_W   = 14.0f;
    constexpr float TOP_Y   = 0.0f;
    constexpr float BOTTOM  = 920.0f;  // stop just above the status bar at y=930
    float           barX    = (m_attackingTeam == 0) ? 0.0f : 1920.0f - BAR_W;

    auto bar = std::make_shared<RenderShape>();
    bar->m_type   = ShapeType::Box;
    bar->m_color  = c;
    bar->m_x      = barX;
    bar->m_y      = TOP_Y;
    bar->m_z      = Z_TOP_STRIP + 5;
    bar->m_width  = BAR_W;
    bar->m_height = BOTTOM - TOP_Y;
    renderQueueAdd(fid, bar);
}


void DartfleetGame::renderMissX(float x, float y, uint32_t z)
{
    FrameID fid = getFrameId();
    std::string sym = "X";
    TTF_Font* font = getFont(m_fontId);
    int w = 0, h = 0;
    if(font) TTF_GetStringSize(font, sym.c_str(), 0, &w, &h);
    float scale = 1.1f;

    auto txt = std::make_shared<RenderText>();
    txt->m_text     = sym;
    txt->m_color    = COLOR_MISS_PURPLE;
    txt->m_fontId   = m_fontId;
    txt->m_rotation = 0.0f;
    txt->m_scaleX   = scale;
    txt->m_scaleY   = scale;
    txt->m_x        = x - static_cast<float>(w) * 0.5f * scale;
    txt->m_y        = y - static_cast<float>(h) * 0.5f * scale;
    txt->m_z        = z;
    renderQueueAdd(fid, txt);
}


// ============================================================================
// Rendering — input hints
// ============================================================================

void DartfleetGame::renderPlacementHints()
{
    // Use the W icon (kbd) / left stick (gamepad) to represent move; D-pad
    // also works on the gamepad side. Q/E and LB/RB advertise both rotation
    // directions explicitly.
    std::vector<InputHint> hints = {
        { SDLK_W,         GAMEPAD_ICON_LEFT_STICK,           "WASD / stick - move ship" },
        { SDLK_Q,         SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,  "rotate left"              },
        { SDLK_E,         SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, "rotate right"             },
        { SDLK_RETURN,    SDL_GAMEPAD_BUTTON_SOUTH,          "confirm ship"             },
        { SDLK_BACKSPACE, SDL_GAMEPAD_BUTTON_EAST,           "undo last"                },
        { SDLK_SPACE,     SDL_GAMEPAD_BUTTON_NORTH,          "finish placement"         },
    };
    m_inputHints.render(getFrameId(), m_fontId, 30.0f, HINTS_Y, Z_HINTS, hints);
}


void DartfleetGame::renderGameOverHints()
{
    std::vector<InputHint> hints = {
        { SDLK_UP,     SDL_GAMEPAD_BUTTON_DPAD_UP,    "select"  },
        { SDLK_RETURN, SDL_GAMEPAD_BUTTON_SOUTH,      "confirm" },
    };
    m_inputHints.render(getFrameId(), m_fontId, 30.0f, HINTS_Y,
                        GameLayout::OVERLAY_Z + 5, hints);
}


// ============================================================================
// Rendering — overlays (no peek, banners, game over)
// ============================================================================

void DartfleetGame::renderNoPeekOverlay(uint8_t teamLeavingIdx)
{
    FrameID fid = getFrameId();

    // Full-screen black panel
    auto bg = std::make_shared<RenderShape>();
    bg->m_type   = ShapeType::Box;
    bg->m_color  = { 0, 0, 0 };
    bg->m_x      = 0.0f;
    bg->m_y      = 0.0f;
    bg->m_z      = Z_NO_PEEK;
    bg->m_width  = 1920.0f;
    bg->m_height = 1080.0f;
    renderQueueAdd(fid, bg);

    uint8_t placingTeamIdx = (teamLeavingIdx == 0) ? 1 : 0;
    Color   placingColor   = (placingTeamIdx == 0) ? COLOR_TEAM0 : COLOR_TEAM1;

    std::string nameHeading  = m_teamNames[placingTeamIdx];
    std::string callToAction = "Place Your Battleships";
    std::string noPeekLine   = m_teamNames[teamLeavingIdx] + ", no peeking!";
    std::string prompt       = "Press any button to begin.";

    auto centeredText = [&](const std::string& s, float y, float scale, Color c, FontID f) {
        TTF_Font* font = getFont(f);
        int w = 0, h = 0;
        if(font) TTF_GetStringSize(font, s.c_str(), 0, &w, &h);

        auto txt = std::make_shared<RenderText>();
        txt->m_text     = s;
        txt->m_color    = c;
        txt->m_fontId   = f;
        txt->m_rotation = 0.0f;
        txt->m_scaleX   = scale;
        txt->m_scaleY   = scale;
        txt->m_x        = 960.0f - static_cast<float>(w) * 0.5f * scale;
        txt->m_y        = y;
        txt->m_z        = Z_NO_PEEK + 1;
        renderQueueAdd(fid, txt);
    };

    // Largest: the placing team's name.
    centeredText(nameHeading,  300.0f, 1.0f,  placingColor, m_largeFontId);
    // Big: the call to action (large font, slightly smaller scale).
    centeredText(callToAction, 470.0f, 0.7f,  COLOR_WHITE,  m_largeFontId);
    // Small: the no-peeking line and the input prompt.
    centeredText(noPeekLine,   620.0f, 1.0f,  COLOR_DIM,    m_fontId);
    centeredText(prompt,       700.0f, 0.85f, COLOR_DIM,    m_fontId);
}


void DartfleetGame::renderConfigError()
{
    FrameID fid = getFrameId();

    auto bg = std::make_shared<RenderShape>();
    bg->m_type   = ShapeType::Box;
    bg->m_color  = { 0, 0, 0 };
    bg->m_x      = 0.0f;
    bg->m_y      = 0.0f;
    bg->m_z      = Z_NO_PEEK;
    bg->m_width  = 1920.0f;
    bg->m_height = 1080.0f;
    renderQueueAdd(fid, bg);

    auto centeredText = [&](const std::string& s, float y, float scale, Color c, FontID f) {
        TTF_Font* font = getFont(f);
        int w = 0, h = 0;
        if(font) TTF_GetStringSize(font, s.c_str(), 0, &w, &h);

        auto txt = std::make_shared<RenderText>();
        txt->m_text     = s;
        txt->m_color    = c;
        txt->m_fontId   = f;
        txt->m_rotation = 0.0f;
        txt->m_scaleX   = scale;
        txt->m_scaleY   = scale;
        txt->m_x        = 960.0f - static_cast<float>(w) * 0.5f * scale;
        txt->m_y        = y;
        txt->m_z        = Z_NO_PEEK + 1;
        renderQueueAdd(fid, txt);
    };

    centeredText("Configuration Error",      380.0f, 0.6f,  COLOR_INVALID, m_largeFontId);
    centeredText(m_configErrorMsg,           520.0f, 1.0f,  COLOR_WHITE,   m_fontId);
    centeredText("Press any button to return to the menu.",
                                              600.0f, 0.85f, COLOR_DIM,    m_fontId);
}


void DartfleetGame::renderFirstTeamRevealBanner()
{
    Color color = (m_attackingTeam == 0) ? COLOR_TEAM0 : COLOR_TEAM1;
    renderAnnouncementBanner(getFrameId(), m_largeFontId,
                             teamName(m_attackingTeam),
                             "FIRES FIRST!",
                             color);
}


void DartfleetGame::renderShipSunkBanner()
{
    std::string heading = teamName(m_lastSunkTeamIdx);
    std::string sizeStr = std::to_string(m_teams[m_lastSunkTeamIdx].ships[m_lastSunkShipIdx].size);
    std::string msg     = "BB-" + sizeStr + " SUNK!";
    renderAnnouncementBanner(getFrameId(), m_largeFontId, heading, msg, COLOR_HIT_ORANGE);
}


void DartfleetGame::renderGameOver()
{
    renderGameOverOverlay(getFrameId(), m_largeFontId, m_fontId,
                          teamName(m_winnerTeamIdx), m_gameOverCursor);
}
