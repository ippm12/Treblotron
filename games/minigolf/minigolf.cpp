/**
 * minigolf.cpp
 */

#include "minigolf.hpp"

#include "game_lib/game_helpers.hpp"
#include "game_lib/components/render_shape.hpp"
#include "game_lib/components/render_text.hpp"
#include "frame/render_queue.hpp"
#include "frame/frame.hpp"
#include "players/players.hpp"
#include "vision/vision.hpp"

#include <SDL3/SDL_keycode.h>
#include <SDL3/SDL_gamepad.h>
#include <SDL3_ttf/SDL_ttf.h>

#include <algorithm>
#include <cmath>
#include <memory>


namespace MiniGolf
{

// ============================================================================
// Tuning constants
// ============================================================================

namespace {

// Layout — course view fills the left ~1410 px; the standard scoreboard
// panel (defined in GameLayout::RIGHT_PANEL_X = 1410, W = 480) sits to
// its right, mirroring x01 / cricket.
constexpr float COURSE_VIEW_X = 0.0f;
constexpr float COURSE_VIEW_Y = 80.0f;       // leave a strip on top for hole banner
constexpr float COURSE_VIEW_W = 1410.0f;
constexpr float COURSE_VIEW_H = 920.0f;

// Z-ordering inside the course view
constexpr uint32_t Z_FELT      = 1;
constexpr uint32_t Z_HASH      = 5;
constexpr uint32_t Z_WALL      = 10;
constexpr uint32_t Z_CUP       = 15;
constexpr uint32_t Z_BALL      = 20;
constexpr uint32_t Z_AIM_ARROW = 25;
constexpr uint32_t Z_BANNER    = 200;

// Visual styling
const Color FELT_COLOR        = {  35, 110,  55 };
const Color WALL_COLOR        = { 100,  70,  40 };
const Color CUP_COLOR         = {  10,  10,  10 };
const Color CUP_RIM_COLOR     = { 230, 220, 180 };
const Color HASH_COLOR        = { 220, 220, 220 };
const Color HASH_FAINT_COLOR  = { 110, 130, 110 };
const Color ARROW_COLOR       = { 255, 240, 100 };
const Color BANNER_BG_COLOR   = {  20,  25,  30 };
const Color BANNER_TEXT_COLOR = { 240, 240, 240 };

// Stroke power → impulse mapping. normalizedRadius in [0, 1].
//   impulse_pxps = MIN + (MAX - MIN) * pow(r, CURVE)
// 1300 px/s with our 100 ppm → 13 m/s, plenty to ricochet.
constexpr float STROKE_MIN_SPEED_PXPS  = 250.0f;
constexpr float STROKE_MAX_SPEED_PXPS  = 1500.0f;
constexpr float STROKE_POWER_CURVE     = 1.4f;

// Settle detection
constexpr float SETTLE_SPEED_PXPS = 8.0f;   // below this, ball is "stopped"
constexpr float SETTLE_HOLD_SECS  = 0.35f;  // must be slow this long

// Banner timings
constexpr float HOLE_INTRO_SECS       = 1.6f;
constexpr float HOLE_TRANSITION_SECS  = 1.6f;

// Each player gets up to 3 throws per turn before the next player goes
// (capped lower if the stroke cap would be exceeded).
constexpr uint8_t MAX_THROWS_PER_TURN = 3;

// Aim arrow
constexpr float AIM_ARROW_FADE_SECS = 0.5f;

// Pixels per metre — keep balls/walls in a sensible Box2D scale.
constexpr float WORLD_PIXELS_PER_METER = 100.0f;

// Hash compass — twenty 18° wedges around the active ball. Ticks sit at
// segment boundaries; labels at segment centres show the standard
// dartboard section number so the player knows what they're aiming at.
constexpr int   HASH_TICK_COUNT         = 20;
constexpr float HASH_INNER_RADIUS_PX    = 80.0f;
constexpr float HASH_OUTER_RADIUS_PX    = 280.0f;
constexpr float HASH_THICKNESS_PX       = 4.0f;
constexpr float HASH_LABEL_RADIUS_PX    = 320.0f;
constexpr float HASH_LABEL_TEXT_SCALE   = 0.55f;

// Standard dartboard section numbering, clockwise from the top (the 20).
// Index i sits at angle (-90° + i*18°) in screen-space polar coordinates
// (matching the codebase convention where +sin(angle) points down).
constexpr std::array<uint8_t, 20> DARTBOARD_LAYOUT = {{
    20, 1, 18, 4, 13, 6, 10, 15, 2, 17,
     3, 19, 7, 16, 8, 11, 14,  9, 12,  5
}};

}  // anonymous


// ============================================================================
// Construction & lifecycle
// ============================================================================

MiniGolfGame::MiniGolfGame(CourseId courseId)
    : Game("Mini Golf"),
      m_courseId(courseId)
{
}


Status MiniGolfGame::init(FrameID frameId)
{
    m_frameId = frameId;

    m_fontId      = loadFont("assets/fonts/Roboto-Regular.ttf", 28.0f);
    m_largeFontId = loadFont("assets/fonts/Roboto-Regular.ttf", 64.0f);

    m_course = buildCourse(m_courseId);

    m_world = std::make_unique<PhysicsWorld>();
    m_world->setPixelsPerMeter(WORLD_PIXELS_PER_METER);

    m_camera.setViewport(COURSE_VIEW_X, COURSE_VIEW_Y, COURSE_VIEW_W, COURSE_VIEW_H);

    // Build per-player state
    const uint8_t playerCount = std::min<uint8_t>(getPlayerCount(), MAX_PLAYERS);
    m_players.clear();
    m_players.resize(playerCount);
    for(uint8_t i = 0; i < playerCount; ++i)
    {
        m_players[i].ballColor = BALL_COLORS[i];
    }

    m_currentHole           = 0;
    m_currentPlayer         = 0;
    m_phase                 = Phase::HoleIntro;
    m_phaseTimer            = 0.0f;
    m_settleTimer           = 0.0f;
    m_lastShotHoled         = false;
    m_aimArrowTimer         = 0.0f;
    m_waitingForCollect     = false;
    m_throwsRemainingInTurn = 0;
    m_lastBoardClear        = isBoardClear();

    buildCurrentHole();
    // First player of the first hole — give them their throws once the
    // intro banner ends. We seed it now so the bar shows the right count
    // during the banner.
    m_throwsRemainingInTurn = throwsAvailableForPlayer(m_currentPlayer);

    return STATUS_OK;
}


void MiniGolfGame::shutdown()
{
    teardownCurrentHole();
    m_world.reset();

    if(m_largeFontId != INVALID_FONT_ID) { unloadFont(m_largeFontId); m_largeFontId = INVALID_FONT_ID; }
    if(m_fontId      != INVALID_FONT_ID) { unloadFont(m_fontId);      m_fontId      = INVALID_FONT_ID; }
}


GameBarInfo MiniGolfGame::getBarInfo() const
{
    const bool gameOver = (m_phase == Phase::GameOver);
    if(gameOver)
    {
        return makeBarInfo(true, false, 0, 0, "");
    }

    if((m_phase == Phase::Aiming || m_phase == Phase::BallInMotion
        || m_phase == Phase::HoleIntro)
       && !m_waitingForCollect)
    {
        std::string status = "Hole " + std::to_string(m_currentHole + 1)
                           + "/" + std::to_string(HOLES_PER_GAME);
        return makeBarInfo(false, false, m_currentPlayer,
                           m_throwsRemainingInTurn, status);
    }

    // Ball moving / banner shown / waiting for collect — show "Collect" so
    // the user pulls the dart, since we model 1 dart = 1 stroke.
    if(m_waitingForCollect)
    {
        return makeBarInfo(false, true, m_currentPlayer, 0, "");
    }

    return makeBarInfo(false, false, m_currentPlayer, 0, "");
}


uint8_t MiniGolfGame::getMaxPlayers() const
{
    return MAX_PLAYERS;
}


// ============================================================================
// Hole build / teardown
// ============================================================================

void MiniGolfGame::buildCurrentHole()
{
    const CourseHole& h = m_course.holes[m_currentHole];

    // ---- Camera bounds ----
    m_camera.setWorldBounds(h.areaTopLeft.x, h.areaTopLeft.y,
                            h.areaBottomRight.x, h.areaBottomRight.y);
    m_camera.setCenter(0.5f * (h.areaTopLeft.x + h.areaBottomRight.x),
                       0.5f * (h.areaTopLeft.y + h.areaBottomRight.y));

    // ---- Boundary walls (auto-built from area rect) ----
    const float minX = h.areaTopLeft.x;
    const float minY = h.areaTopLeft.y;
    const float maxX = h.areaBottomRight.x;
    const float maxY = h.areaBottomRight.y;
    const float wallThick = 30.0f;

    auto addBoundary = [&](float cx, float cy, float w, float bh) {
        m_wallBodies.push_back(
            createStaticBox(*m_world, cx, cy, w, bh, nullptr,
                            { 1.0f, 0.4f, 0.5f }));
    };
    addBoundary(0.5f * (minX + maxX), minY - 0.5f * wallThick,
                (maxX - minX) + 2.0f * wallThick, wallThick);          // top
    addBoundary(0.5f * (minX + maxX), maxY + 0.5f * wallThick,
                (maxX - minX) + 2.0f * wallThick, wallThick);          // bottom
    addBoundary(minX - 0.5f * wallThick, 0.5f * (minY + maxY),
                wallThick, (maxY - minY));                              // left
    addBoundary(maxX + 0.5f * wallThick, 0.5f * (minY + maxY),
                wallThick, (maxY - minY));                              // right

    for(const auto& w : h.walls)
    {
        m_wallBodies.push_back(
            createStaticBox(*m_world, w.centerX, w.centerY,
                            w.width, w.height, nullptr,
                            { 1.0f, 0.4f, 0.5f }));
    }

    // ---- Cup sensor ----
    m_cupUserData.kind    = PhysicsBodyKind::Cup;
    m_cupUserData.payload = nullptr;
    m_cupBody = createStaticCircleSensor(*m_world,
                                         h.cupPos.x, h.cupPos.y,
                                         h.cupRadius,
                                         &m_cupUserData);

    // ---- Player balls ----
    // Cluster them slightly so they don't all spawn in identical positions
    // (Box2D resolves overlap, but it adds an unwanted impulse). Spread
    // across a 90px-wide arc behind the start.
    const uint8_t n = static_cast<uint8_t>(m_players.size());
    for(uint8_t i = 0; i < n; ++i)
    {
        const float t   = (n == 1) ? 0.0f : (static_cast<float>(i) / (n - 1) - 0.5f);
        const float spawnX = h.startPos.x + t * 90.0f;
        const float spawnY = h.startPos.y;

        m_players[i].ballUserData.kind    = PhysicsBodyKind::Ball;
        m_players[i].ballUserData.payload = reinterpret_cast<void*>(static_cast<uintptr_t>(i));

        m_players[i].ballBody = createDynamicCircle(
            *m_world, spawnX, spawnY,
            BALL_RADIUS_PX, &m_players[i].ballUserData,
            { 1.0f, 0.3f, 0.4f }, /*linearDamping*/ 0.9f);

        m_players[i].finishedHole[m_currentHole] = false;
        m_players[i].holedOut[m_currentHole]     = false;
        m_players[i].rotationRadians             = 0.0f;
    }

    // First player who hasn't finished
    m_currentPlayer         = 0;
    m_phase                 = Phase::HoleIntro;
    m_phaseTimer            = 0.0f;
    m_settleTimer           = 0.0f;
    m_lastShotHoled         = false;
    m_aimArrowTimer         = 0.0f;
    m_waitingForCollect     = false;
    m_throwsRemainingInTurn = throwsAvailableForPlayer(m_currentPlayer);
}


uint8_t MiniGolfGame::throwsAvailableForPlayer(uint8_t playerIdx) const
{
    if(playerIdx >= m_players.size()) return 0;
    const uint8_t used = m_players[playerIdx].strokes[m_currentHole];
    if(used >= STROKE_CAP) return 0;
    const uint8_t remaining = static_cast<uint8_t>(STROKE_CAP - used);
    return std::min<uint8_t>(MAX_THROWS_PER_TURN, remaining);
}


void MiniGolfGame::teardownCurrentHole()
{
    if(!m_world) return;

    for(b2BodyId b : m_wallBodies)
    {
        if(b2Body_IsValid(b)) b2DestroyBody(b);
    }
    m_wallBodies.clear();

    if(b2Body_IsValid(m_cupBody))
    {
        b2DestroyBody(m_cupBody);
        m_cupBody = b2_nullBodyId;
    }

    for(auto& p : m_players)
    {
        if(b2Body_IsValid(p.ballBody))
        {
            b2DestroyBody(p.ballBody);
            p.ballBody = b2_nullBodyId;
        }
    }
}


void MiniGolfGame::resetBallsToStart()
{
    // Currently unused — buildCurrentHole creates fresh balls. Kept for
    // when we want mid-hole continuity (e.g. after a stroke cap).
}


// ============================================================================
// Update (phase machine)
// ============================================================================

void MiniGolfGame::update(float deltaTime)
{
    // 1) Always step physics — even between phases — so balls finish
    //    settling visibly during the hole-transition banner.
    if(m_world) m_world->step(deltaTime);

    // 2) Drain landed counter (we don't use it; one DartPosition == one stroke).
    (void)consumeDartLandedCount();

    // 3) Detect dart-pull edge: board went from not-clear to clear. This
    //    is the canonical "turn over, next player" signal — works for
    //    both the simulator's Collect button and a real dart pull.
    const bool boardClearNow = isBoardClear();
    const bool collectEdge   = boardClearNow && !m_lastBoardClear;
    m_lastBoardClear         = boardClearNow;

    if(m_waitingForCollect && collectEdge)
    {
        m_waitingForCollect = false;
        // If we were between holes, advanceToNextHole takes us to the next
        // hole's intro. Otherwise the next player's turn begins.
        if(m_phase == Phase::HoleTransition)
        {
            advanceToNextHole();
        }
        else
        {
            beginNextTurn();
        }
    }

    // 4) Phase machine.
    switch(m_phase)
    {
        case Phase::HoleIntro:
        {
            // Discard any stray darts queued before the player is ready.
            DartPosition d;
            while(popDartPosition(d)) {}
            m_phaseTimer += deltaTime;
            if(m_phaseTimer >= HOLE_INTRO_SECS)
            {
                m_phase      = Phase::Aiming;
                m_phaseTimer = 0.0f;
            }
            break;
        }

        case Phase::Aiming:
        {
            if(m_waitingForCollect)
            {
                // Turn ended; player needs to pull their darts before the
                // next turn starts. Drain any stray darts to keep the queue
                // clean.
                DartPosition d;
                while(popDartPosition(d)) {}
                break;
            }

            if(m_aimArrowTimer > 0.0f)
            {
                m_aimArrowTimer = std::max(0.0f, m_aimArrowTimer - deltaTime);
            }

            if(m_throwsRemainingInTurn == 0)
            {
                // Should not normally happen — endCurrentTurn flips us to
                // waitingForCollect. Defensive drain.
                DartPosition d;
                while(popDartPosition(d)) {}
                break;
            }

            DartPosition pos;
            if(popDartPosition(pos))
            {
                processDart(pos);
                // Drain extras: only one stroke per Aiming entry. The
                // settle/pollSensorEvents path will return us to Aiming
                // for the next throw of this turn.
                DartPosition extra;
                while(popDartPosition(extra)) {}
            }
            break;
        }

        case Phase::BallInMotion:
            pollSensorEvents();
            updateBallMotion(deltaTime);
            break;

        case Phase::HoleTransition:
        {
            DartPosition d;
            while(popDartPosition(d)) {}
            m_phaseTimer += deltaTime;
            // Banner timer no longer auto-advances — we wait on the
            // collect edge so the next hole begins after the player
            // pulls their darts. If the board is already clear (e.g.
            // ball never moved), advance once the banner expires as a
            // safety net so the game can't get stuck.
            if(m_phaseTimer >= HOLE_TRANSITION_SECS && boardClearNow)
            {
                m_waitingForCollect = false;
                advanceToNextHole();
            }
            break;
        }

        case Phase::GameOver:
        {
            DartPosition d;
            while(popDartPosition(d)) {}
            break;
        }
    }

    // Camera follows the active player's ball.
    if(m_phase != Phase::GameOver && m_currentPlayer < m_players.size())
    {
        b2BodyId b = m_players[m_currentPlayer].ballBody;
        if(b2Body_IsValid(b))
        {
            float bx = 0.0f, by = 0.0f;
            getBodyPositionPx(*m_world, b, bx, by);
            m_camera.follow(bx, by, 200.0f);
        }
    }
}


// ============================================================================
// Stroke handling
// ============================================================================

void MiniGolfGame::processDart(const DartPosition& pos)
{
    if(m_currentPlayer >= m_players.size()) return;
    PlayerState& p = m_players[m_currentPlayer];
    if(!b2Body_IsValid(p.ballBody)) return;
    if(p.holedOut[m_currentHole]) return;  // already holed; wait for hole to advance

    // Convert polar (angle deg, normalizedRadius [0,1]) to a screen-space
    // direction + speed. Convention matches existing helpers
    // (renderHitMarkers): y uses +sin, so angle=0 → +X (right) and
    // angle=90 → +Y (down). If gameplay testing reveals the ball flies
    // 180° opposite where the player aimed, flip sign here, not in
    // course coordinates.
    const float angleRad = pos.angle * (3.14159265358979f / 180.0f);
    const float r        = std::clamp(pos.normalizedRadius, 0.0f, 1.0f);
    const float power    = std::pow(r, STROKE_POWER_CURVE);
    const float speedPx  = STROKE_MIN_SPEED_PXPS
                         + (STROKE_MAX_SPEED_PXPS - STROKE_MIN_SPEED_PXPS) * power;
    const float dx       = std::cos(angleRad);
    const float dy       = std::sin(angleRad);

    const float vx = dx * speedPx;
    const float vy = dy * speedPx;
    applyImpulsePxPerSec(*m_world, p.ballBody, vx, vy);

    // Aim arrow records origin so it renders in world space even as the
    // ball moves. Length scales with power so the user sees how hard
    // they hit it.
    float bx = 0.0f, by = 0.0f;
    getBodyPositionPx(*m_world, p.ballBody, bx, by);
    m_aimArrowOriginX  = bx;
    m_aimArrowOriginY  = by;
    m_aimArrowDirX     = dx;
    m_aimArrowDirY     = dy;
    m_aimArrowLengthPx = 60.0f + 220.0f * r;
    m_aimArrowTimer    = AIM_ARROW_FADE_SECS;

    p.strokes[m_currentHole] = static_cast<uint8_t>(p.strokes[m_currentHole] + 1);
    if(m_throwsRemainingInTurn > 0) m_throwsRemainingInTurn--;
    m_phase         = Phase::BallInMotion;
    m_settleTimer   = 0.0f;
    m_lastShotHoled = false;
}


void MiniGolfGame::pollSensorEvents()
{
    if(!m_world) return;
    b2SensorEvents events = b2World_GetSensorEvents(m_world->id());
    for(int i = 0; i < events.beginCount; ++i)
    {
        const b2SensorBeginTouchEvent& ev = events.beginEvents[i];

        // Only the cup is a sensor in mini golf — confirm via userData.
        void* sensorUd = b2Shape_GetUserData(ev.sensorShapeId);
        const PhysicsUserData* sd = static_cast<const PhysicsUserData*>(sensorUd);
        if(!sd || sd->kind != PhysicsBodyKind::Cup) continue;

        // Visitor must be a ball; payload encodes the player index.
        void* visitorUd = b2Shape_GetUserData(ev.visitorShapeId);
        const PhysicsUserData* vd = static_cast<const PhysicsUserData*>(visitorUd);
        if(!vd || vd->kind != PhysicsBodyKind::Ball) continue;

        const uintptr_t playerIdx = reinterpret_cast<uintptr_t>(vd->payload);
        if(playerIdx >= m_players.size()) continue;

        PlayerState& p = m_players[playerIdx];
        if(p.holedOut[m_currentHole]) continue;
        p.holedOut[m_currentHole] = true;
        if(b2Body_IsValid(p.ballBody))
        {
            freezeBody(p.ballBody);
        }
        if(playerIdx == m_currentPlayer)
        {
            m_lastShotHoled = true;
        }
    }
}


void MiniGolfGame::updateBallMotion(float deltaTime)
{
    if(m_currentPlayer >= m_players.size()) return;
    PlayerState& p = m_players[m_currentPlayer];

    // Update ball roll-rotation accumulator (radians). This drives a
    // future textured ball; for v1 it's invisible on a solid circle.
    if(b2Body_IsValid(p.ballBody))
    {
        const float speedPx = getBodySpeedPx(*m_world, p.ballBody);
        // dθ = (v / r). Treat r as ball radius in pixels, not metres —
        // the ratio is unit-agnostic.
        p.rotationRadians += (speedPx / BALL_RADIUS_PX) * deltaTime;
    }

    const bool slow = m_lastShotHoled
                   || (b2Body_IsValid(p.ballBody)
                       && getBodySpeedPx(*m_world, p.ballBody) < SETTLE_SPEED_PXPS);
    if(slow)
    {
        m_settleTimer += deltaTime;
        if(m_settleTimer >= SETTLE_HOLD_SECS)
        {
            onBallSettled();
        }
    }
    else
    {
        m_settleTimer = 0.0f;
    }
}


void MiniGolfGame::onBallSettled()
{
    if(m_currentPlayer >= m_players.size()) return;
    PlayerState& p = m_players[m_currentPlayer];
    if(b2Body_IsValid(p.ballBody)) freezeBody(p.ballBody);

    m_settleTimer = 0.0f;

    const bool capped = (p.strokes[m_currentHole] >= STROKE_CAP);
    const bool holed  = p.holedOut[m_currentHole];
    if(capped || holed)
    {
        p.finishedHole[m_currentHole] = true;
        endCurrentTurn();
        return;
    }

    // Still throws remaining in this turn AND ball not holed/capped:
    // continue same turn. No collect needed between throws within a turn.
    if(m_throwsRemainingInTurn > 0)
    {
        m_phase = Phase::Aiming;
        return;
    }

    // Used all 3 throws this turn but didn't finish the hole — pass to
    // the next player after the player collects their darts.
    endCurrentTurn();
}


void MiniGolfGame::endCurrentTurn()
{
    if(allPlayersFinishedHole())
    {
        m_phase      = Phase::HoleTransition;
        m_phaseTimer = 0.0f;
    }
    else
    {
        advancePlayerWithinHole();
        m_phase = Phase::Aiming;
    }
    m_throwsRemainingInTurn = 0;

    // If the board is already clear (e.g. simulator with no darts on it,
    // or onMissedThrow with bounce-out), don't make the player pull
    // nothing — start the next turn immediately.
    if(isBoardClear())
    {
        m_waitingForCollect = false;
        if(m_phase == Phase::HoleTransition)
        {
            advanceToNextHole();
        }
        else
        {
            beginNextTurn();
        }
    }
    else
    {
        m_waitingForCollect = true;
    }
}


void MiniGolfGame::beginNextTurn()
{
    if(m_currentPlayer >= m_players.size()) return;
    m_throwsRemainingInTurn = throwsAvailableForPlayer(m_currentPlayer);
    m_phase = Phase::Aiming;
}


void MiniGolfGame::advancePlayerWithinHole()
{
    const uint8_t n = static_cast<uint8_t>(m_players.size());
    for(uint8_t step = 1; step <= n; ++step)
    {
        uint8_t cand = (m_currentPlayer + step) % n;
        if(!m_players[cand].finishedHole[m_currentHole])
        {
            m_currentPlayer = cand;
            return;
        }
    }
}


bool MiniGolfGame::allPlayersFinishedHole() const
{
    for(const auto& p : m_players)
    {
        if(!p.finishedHole[m_currentHole]) return false;
    }
    return true;
}


void MiniGolfGame::advanceToNextHole()
{
    teardownCurrentHole();
    m_currentHole++;
    if(m_currentHole >= HOLES_PER_GAME)
    {
        m_phase             = Phase::GameOver;
        m_phaseTimer        = 0.0f;
        m_gameOverCursor    = 0;
        m_waitingForCollect = false;
        return;
    }
    buildCurrentHole();
}


// ============================================================================
// Input
// ============================================================================

void MiniGolfGame::onKeyDown(uint32_t keycode)
{
    if(m_phase == Phase::GameOver)
    {
        GameOverAction action = handleGameOverKey(keycode, m_gameOverCursor);
        (void)action;
        return;
    }
    (void)keycode;
}


void MiniGolfGame::onGamepadButton(uint8_t button, bool pressed)
{
    if(!pressed) return;
    if(m_phase == Phase::GameOver)
    {
        GameOverAction action = handleGameOverGamepad(button, m_gameOverCursor);
        (void)action;
        return;
    }
    (void)button;
}


void MiniGolfGame::onMissedThrow()
{
    // Bounced out / not detected: count one stroke without moving the ball.
    if(m_waitingForCollect) return;
    if(m_currentPlayer >= m_players.size()) return;
    if(m_throwsRemainingInTurn == 0) return;

    PlayerState& p = m_players[m_currentPlayer];
    p.strokes[m_currentHole] = static_cast<uint8_t>(
        std::min<uint8_t>(STROKE_CAP, p.strokes[m_currentHole] + 1));
    m_throwsRemainingInTurn--;

    const bool capped = (p.strokes[m_currentHole] >= STROKE_CAP);
    if(capped)
    {
        p.finishedHole[m_currentHole] = true;
        endCurrentTurn();
        return;
    }
    if(m_throwsRemainingInTurn == 0)
    {
        endCurrentTurn();
    }
}


// ============================================================================
// Render
// ============================================================================

void MiniGolfGame::render()
{
    if(m_phase == Phase::GameOver)
    {
        renderGameOverScreen();
        renderScorecardPanel();
        return;
    }

    renderCourse();
    renderHashCompass();
    renderBalls();
    renderAimArrow();
    renderHoleBanner();
    renderScorecardPanel();
}


void MiniGolfGame::renderCourse()
{
    FrameID fid = getFrameId();
    const CourseHole& h = m_course.holes[m_currentHole];

    // Floor: fill course view area with felt colour.
    auto floor = std::make_shared<RenderShape>();
    floor->m_type   = ShapeType::Box;
    floor->m_color  = FELT_COLOR;
    floor->m_x      = COURSE_VIEW_X;
    floor->m_y      = COURSE_VIEW_Y;
    floor->m_z      = Z_FELT;
    floor->m_width  = COURSE_VIEW_W;
    floor->m_height = COURSE_VIEW_H;
    renderQueueAdd(fid, floor);

    // Walls — draw each as a screen-space box. We reuse the course's
    // wall list (interior obstacles) plus the four implicit boundaries.
    auto drawWall = [&](float cx, float cy, float w, float ht) {
        float sx = 0.0f, sy = 0.0f;
        m_camera.worldToScreen(cx, cy, sx, sy);
        const float sw = m_camera.worldToScreenLength(w);
        const float sh = m_camera.worldToScreenLength(ht);
        auto wall = std::make_shared<RenderShape>();
        wall->m_type   = ShapeType::Box;
        wall->m_color  = WALL_COLOR;
        wall->m_x      = sx - 0.5f * sw;
        wall->m_y      = sy - 0.5f * sh;
        wall->m_z      = Z_WALL;
        wall->m_width  = sw;
        wall->m_height = sh;
        renderQueueAdd(fid, wall);
    };

    // Boundary walls — same rect we built bodies from in buildCurrentHole.
    const float minX = h.areaTopLeft.x;
    const float minY = h.areaTopLeft.y;
    const float maxX = h.areaBottomRight.x;
    const float maxY = h.areaBottomRight.y;
    const float wt   = 30.0f;
    drawWall(0.5f * (minX + maxX), minY - 0.5f * wt,
             (maxX - minX) + 2.0f * wt, wt);
    drawWall(0.5f * (minX + maxX), maxY + 0.5f * wt,
             (maxX - minX) + 2.0f * wt, wt);
    drawWall(minX - 0.5f * wt, 0.5f * (minY + maxY),
             wt, (maxY - minY));
    drawWall(maxX + 0.5f * wt, 0.5f * (minY + maxY),
             wt, (maxY - minY));

    for(const auto& w : h.walls)
    {
        drawWall(w.centerX, w.centerY, w.width, w.height);
    }

    // Cup: dark fill + light rim.
    {
        float sx = 0.0f, sy = 0.0f;
        m_camera.worldToScreen(h.cupPos.x, h.cupPos.y, sx, sy);
        const float r = m_camera.worldToScreenLength(h.cupRadius);

        auto rim = std::make_shared<RenderShape>();
        rim->m_type  = ShapeType::Circle;
        rim->m_color = CUP_RIM_COLOR;
        rim->m_x     = sx;
        rim->m_y     = sy;
        rim->m_z     = Z_CUP;
        rim->m_width = 2.0f * (r + 3.0f);
        rim->m_height = 0.0f;
        renderQueueAdd(fid, rim);

        auto cup = std::make_shared<RenderShape>();
        cup->m_type  = ShapeType::Circle;
        cup->m_color = CUP_COLOR;
        cup->m_x     = sx;
        cup->m_y     = sy;
        cup->m_z     = Z_CUP + 1;
        cup->m_width = 2.0f * r;
        cup->m_height = 0.0f;
        renderQueueAdd(fid, cup);
    }
}


void MiniGolfGame::renderHashCompass()
{
    if(m_phase != Phase::Aiming) return;
    if(m_currentPlayer >= m_players.size()) return;

    FrameID fid = getFrameId();
    b2BodyId b = m_players[m_currentPlayer].ballBody;
    if(!b2Body_IsValid(b)) return;

    float bxW = 0.0f, byW = 0.0f;
    getBodyPositionPx(*m_world, b, bxW, byW);
    float bxS = 0.0f, byS = 0.0f;
    m_camera.worldToScreen(bxW, byW, bxS, byS);

    const Color tickColor = (m_aimArrowTimer > 0.0f) ? HASH_FAINT_COLOR : HASH_COLOR;
    const float tickLen   = HASH_OUTER_RADIUS_PX - HASH_INNER_RADIUS_PX;
    const float DEG2RAD   = 3.14159265358979f / 180.0f;

    // 1) Tick lines at segment boundaries (between two adjacent sections).
    //    Boundary i lies between section DARTBOARD_LAYOUT[i] (centred at
    //    -90° + i*18°) and section DARTBOARD_LAYOUT[(i+1) % 20] — i.e.
    //    at angle -90° + i*18° + 9°.
    for(int i = 0; i < HASH_TICK_COUNT; ++i)
    {
        const float angDeg = -90.0f + i * 18.0f + 9.0f;
        const float ang    = angDeg * DEG2RAD;
        const float dx     = std::cos(ang);
        const float dy     = std::sin(ang);
        const float midX   = bxS + dx * (HASH_INNER_RADIUS_PX + 0.5f * tickLen);
        const float midY   = byS + dy * (HASH_INNER_RADIUS_PX + 0.5f * tickLen);

        auto tick = std::make_shared<RenderShape>();
        tick->m_type     = ShapeType::Box;
        tick->m_color    = tickColor;
        tick->m_x        = midX - 0.5f * tickLen;
        tick->m_y        = midY - 0.5f * HASH_THICKNESS_PX;
        tick->m_z        = Z_HASH;
        tick->m_width    = tickLen;
        tick->m_height   = HASH_THICKNESS_PX;
        tick->m_rotation = ang;
        renderQueueAdd(fid, tick);
    }

    // 2) Section number labels at segment centres. Each label sits in
    //    the wedge between two ticks and tells the player which
    //    dartboard section to aim at for that direction.
    TTF_Font* font = getFont(m_fontId);
    for(int i = 0; i < HASH_TICK_COUNT; ++i)
    {
        const float angDeg = -90.0f + i * 18.0f;
        const float ang    = angDeg * DEG2RAD;
        const float dx     = std::cos(ang);
        const float dy     = std::sin(ang);
        const float labelX = bxS + dx * HASH_LABEL_RADIUS_PX;
        const float labelY = byS + dy * HASH_LABEL_RADIUS_PX;

        const std::string text = std::to_string(DARTBOARD_LAYOUT[i]);
        int tw = 0, th = 0;
        if(font) TTF_GetStringSize(font, text.c_str(), 0, &tw, &th);
        const float scale = HASH_LABEL_TEXT_SCALE;

        auto label = std::make_shared<RenderText>();
        label->m_text     = text;
        label->m_color    = tickColor;
        label->m_fontId   = m_fontId;
        label->m_rotation = 0.0f;
        label->m_scaleX   = scale;
        label->m_scaleY   = scale;
        label->m_x        = labelX - tw * scale * 0.5f;
        label->m_y        = labelY - th * scale * 0.5f;
        label->m_z        = Z_HASH + 1;
        renderQueueAdd(fid, label);
    }
}


void MiniGolfGame::renderBalls()
{
    FrameID fid = getFrameId();
    for(uint8_t i = 0; i < m_players.size(); ++i)
    {
        const PlayerState& p = m_players[i];
        if(!b2Body_IsValid(p.ballBody)) continue;
        if(p.holedOut[m_currentHole]) continue;  // ball removed visually after holing

        float wx = 0.0f, wy = 0.0f;
        getBodyPositionPx(*m_world, p.ballBody, wx, wy);
        float sx = 0.0f, sy = 0.0f;
        m_camera.worldToScreen(wx, wy, sx, sy);
        const float r = m_camera.worldToScreenLength(BALL_RADIUS_PX);

        const bool isActive = (m_phase == Phase::Aiming) && (i == m_currentPlayer);
        // Pulse the active ball by oscillating its radius.
        float pulseScale = 1.0f;
        if(isActive)
        {
            // Use phaseTimer-independent blink — read SDL ticks via cmath
            // could work, but a simple fmod over time isn't stored. Use
            // a time-of-render proxy: aim arrow timer's complement.
            // Cheap stable pulse based on hole timer ensures it doesn't
            // freeze when nothing else is moving.
            pulseScale = 1.0f + 0.06f
                       * std::sin(m_phaseTimer * 6.0f
                                + m_aimArrowTimer * 6.0f);
        }

        auto ball = std::make_shared<RenderShape>();
        ball->m_type   = ShapeType::Circle;
        ball->m_color  = p.ballColor;
        ball->m_x      = sx;
        ball->m_y      = sy;
        ball->m_z      = Z_BALL + (isActive ? 1u : 0u);
        ball->m_width  = 2.0f * r * pulseScale;
        ball->m_height = 0.0f;
        renderQueueAdd(fid, ball);
    }
}


void MiniGolfGame::renderAimArrow()
{
    if(m_aimArrowTimer <= 0.0f) return;
    FrameID fid = getFrameId();

    const float fade = m_aimArrowTimer / AIM_ARROW_FADE_SECS;
    Color c = ARROW_COLOR;
    c.r = static_cast<uint8_t>(c.r * fade);
    c.g = static_cast<uint8_t>(c.g * fade);
    c.b = static_cast<uint8_t>(c.b * fade);

    float sx0 = 0.0f, sy0 = 0.0f;
    m_camera.worldToScreen(m_aimArrowOriginX, m_aimArrowOriginY, sx0, sy0);
    const float lenS = m_camera.worldToScreenLength(m_aimArrowLengthPx);

    // Render arrow as a rotated thin box, with origin at the ball.
    const float thickness = 6.0f;
    const float ang       = std::atan2(m_aimArrowDirY, m_aimArrowDirX);

    auto shaft = std::make_shared<RenderShape>();
    shaft->m_type     = ShapeType::Box;
    shaft->m_color    = c;
    // Box rotation pivots around its geometric centre. Place the box so
    // that its centre is half-length down the arrow direction from the
    // ball, then m_x/m_y are the unrotated top-left.
    {
        const float midX = sx0 + 0.5f * lenS * m_aimArrowDirX;
        const float midY = sy0 + 0.5f * lenS * m_aimArrowDirY;
        shaft->m_x = midX - 0.5f * lenS;
        shaft->m_y = midY - 0.5f * thickness;
    }
    shaft->m_z      = Z_AIM_ARROW;
    shaft->m_width  = lenS;
    shaft->m_height = thickness;
    shaft->m_rotation = ang;
    renderQueueAdd(fid, shaft);
}


void MiniGolfGame::renderHoleBanner()
{
    FrameID fid = getFrameId();

    std::string text;
    if(m_phase == Phase::HoleIntro)
    {
        text = "Hole " + std::to_string(m_currentHole + 1)
             + " / " + std::to_string(HOLES_PER_GAME);
    }
    else if(m_phase == Phase::HoleTransition)
    {
        text = "Hole " + std::to_string(m_currentHole + 1) + " complete";
    }

    if(text.empty()) return;

    auto bg = std::make_shared<RenderShape>();
    bg->m_type   = ShapeType::Box;
    bg->m_color  = BANNER_BG_COLOR;
    bg->m_x      = 360.0f;
    bg->m_y      = 380.0f;
    bg->m_z      = Z_BANNER;
    bg->m_width  = 700.0f;
    bg->m_height = 160.0f;
    renderQueueAdd(fid, bg);

    TTF_Font* font = getFont(m_largeFontId);
    int tw = 0, th = 0;
    if(font) TTF_GetStringSize(font, text.c_str(), 0, &tw, &th);
    const float scale = 0.6f;

    auto txt = std::make_shared<RenderText>();
    txt->m_text     = text;
    txt->m_color    = BANNER_TEXT_COLOR;
    txt->m_fontId   = m_largeFontId;
    txt->m_rotation = 0.0f;
    txt->m_scaleX   = scale;
    txt->m_scaleY   = scale;
    txt->m_x        = 710.0f - tw * scale * 0.5f;
    txt->m_y        = 430.0f;
    txt->m_z        = Z_BANNER + 1;
    renderQueueAdd(fid, txt);
}


void MiniGolfGame::renderScorecardPanel()
{
    std::vector<ScoreboardEntry> entries;
    entries.reserve(m_players.size());
    for(uint8_t i = 0; i < m_players.size(); ++i)
    {
        const PlayerState& p = m_players[i];
        ScoreboardEntry e;
        const PlayerID pid = getPlayerByIndex(i);
        e.name       = (pid != INVALID_PLAYER_ID) ? getPlayerName(pid)
                                                  : ("Player " + std::to_string(i + 1));
        e.value      = std::to_string(p.totalStrokes());
        e.valueColor = p.ballColor;
        // Detail: strokes on current hole.
        const uint8_t s = p.strokes[m_currentHole];
        e.detailText = "Hole " + std::to_string(m_currentHole + 1)
                     + ": " + std::to_string(s);
        entries.push_back(e);
    }

    renderScoreboardPanel(getFrameId(), m_fontId, entries, m_currentPlayer);
}


void MiniGolfGame::renderGameOverScreen()
{
    // Find the lowest-stroke player as the "winner".
    uint8_t winner = 0;
    uint16_t bestScore = 0xFFFF;
    for(uint8_t i = 0; i < m_players.size(); ++i)
    {
        uint16_t s = m_players[i].totalStrokes();
        if(s < bestScore) { bestScore = s; winner = i; }
    }
    std::string name = "Player " + std::to_string(winner + 1);
    const PlayerID pid = getPlayerByIndex(winner);
    if(pid != INVALID_PLAYER_ID) name = getPlayerName(pid);

    renderGameOverOverlay(getFrameId(), m_largeFontId, m_fontId,
                          name, m_gameOverCursor);
}

}  // namespace MiniGolf
