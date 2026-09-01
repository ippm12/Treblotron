/**
 * minigolf.cpp
 */

#include "minigolf.hpp"

#include "game_lib/game_helpers.hpp"
#include "game_lib/game_manager.hpp"
#include "games/main_menu.hpp"
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

// Layout lives in course_defs.hpp — COURSE_VIEW_* is shared with the course
// authoring side so a hole and the window it is drawn into cannot drift apart.

// Z-ordering inside the course view
constexpr uint32_t Z_FELT      = 1;
constexpr uint32_t Z_HASH      = 5;
constexpr uint32_t Z_WALL      = 10;
constexpr uint32_t Z_CUP       = 15;
constexpr uint32_t Z_TRAIL     = 18;
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

// Rolling resistance of the felt, and the only thing that slows a rolling ball.
// The PhysicsMaterial friction values below are NOT it: the felt is drawn, not
// simulated — there is no floor body — so in this zero-gravity top-down world
// the ball touches nothing as it rolls and surface friction only ever applies
// to a wall scrape.
//
// This is a constant deceleration, applied by applyRollingFriction(), rather
// than Box2D's linearDamping. Damping is exponential: the ball approaches rest
// asymptotically and never actually arrives, so it spends seconds crawling and
// reads as being on ice. Real rolling friction is near-constant and brings the
// ball to a definite stop, which is both more accurate and much crisper — a
// full-power putt now settles in 3.3 s rather than 7.6 s.
//
// Roll distance is speed^2 / (2 * decel), so 750 px/s at 225 px/s^2 covers
// ~1520 px of the 1280 x 800 play area and puts the cup at a dart radius of
// about 0.60 (with STROKE_POWER_CURVE below holding that radius steady).
constexpr float BALL_ROLL_DECEL_PXPS2 = 185.0f;

// Stroke power → impulse mapping. normalizedRadius in [0, 1].
//   impulse_pxps = MIN + (MAX - MIN) * pow(r, CURVE)
//
// MIN is a floor under *every* putt, not a starting point — the curve only ever
// adds to it — so it directly sets the shortest shot the game can produce. Its
// only real job is to stay clear of SETTLE_SPEED_PXPS (8): launch at or below
// that and the ball counts as stopped the moment it leaves, settles after the
// hold, and has moved under 3 px, which reads as a broken stroke rather than a
// gentle one. 20 px/s is 2.5x clear of it and taps the ball about 27 px.
// It was 250, which put a 556 px floor under every putt — 93% of the way to a
// cup 600 px away, so no dart anywhere on the board could leave a short one.
//
// Under constant-deceleration friction roll goes as speed^2, so the floor is
// set by the shortest putt worth having rather than by SETTLE_SPEED_PXPS: at
// 20 px/s the ball would travel under a pixel. 125 px/s taps it about 35 px.
//
// CURVE compensates for that same squaring, and is re-solved whenever the
// deceleration changes so a full-length putt keeps needing the same dart
// radius. At 185 px/s^2, 1.15 puts the cup at r ~ 0.60 with ~15% of the board
// on putts under 300 px. (Leaving it at 0.9 would drag the cup in to r ~ 0.52.)
constexpr float STROKE_MIN_SPEED_PXPS  = 125.0f;
constexpr float STROKE_MAX_SPEED_PXPS  = 750.0f;
constexpr float STROKE_POWER_CURVE     = 1.15f;

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

// Ball trail. Records where each ball has been since the stroke was struck,
// which is what makes the curl around the cup readable — the path bends over
// a few dozen pixels and is hard to see in motion otherwise.
//
// 90 samples at 25 ms covers ~2.2 s, comfortably longer than the ~4 s a
// full-power putt takes to stop, so the tail thins out rather than vanishing
// mid-roll.
constexpr size_t TRAIL_MAX_POINTS   = 90;
constexpr float  TRAIL_SAMPLE_SECS  = 0.025f;

// How far each end of the trail is mixed from the felt toward the ball colour.
// The head stops well short of 1.0 on purpose: at full strength it is the same
// colour as the ball it is attached to and the two read as one blob, which
// costs exactly the separation the trail exists to give.
constexpr float  TRAIL_FADE_TAIL    = 0.20f;
constexpr float  TRAIL_FADE_HEAD    = 0.70f;

/** Blend two colours; t = 0 gives a, t = 1 gives b. Color carries no alpha, so
 *  fading means mixing toward the colour of the surface behind the mark. */
Color lerpColor(Color a, Color b, float t)
{
    t = std::clamp(t, 0.0f, 1.0f);
    auto mix = [t](uint8_t x, uint8_t y) {
        return static_cast<uint8_t>(
            static_cast<float>(x) + (static_cast<float>(y) - static_cast<float>(x)) * t + 0.5f);
    };
    return { mix(a.r, b.r), mix(a.g, b.g), mix(a.b, b.b) };
}

// Pixels per metre — keep balls/walls in a sensible Box2D scale.
constexpr float WORLD_PIXELS_PER_METER = 100.0f;

// Slowest approach to a wall that still bounces, measured along the contact
// normal. Box2D's default threshold is 1 m/s — 100 px/s at the scale above —
// which is inside our normal range of play (strokes are 250..1500 px/s), so
// any shallow-angle hit lost its bounce and skated along the wall instead.
// Kept well above zero so a ball dribbling to a halt against a wall settles
// instead of buzzing; SETTLE_SPEED_PXPS (8) is the speed we call "stopped".
constexpr float WALL_BOUNCE_MIN_SPEED_PXPS = 20.0f;

// Cup capture. The cup is not a trigger volume — a ball drops only if it is
// both centred enough over the mouth and slow enough when it gets there, so a
// quick putt can run across the hole instead of vanishing the moment it
// grazes it.
//
// How centred it has to be scales with pace: at rest anywhere over the mouth
// drops, at CUP_CAPTURE_MAX_SPEED_PXPS it must be nearly dead centre, and past
// that speed it always runs over the top. FALLOFF is the fraction of the cup
// radius taken away at the speed limit.
constexpr float CUP_CAPTURE_MAX_SPEED_PXPS = 400.0f;
constexpr float CUP_CAPTURE_RADIUS_FALLOFF = 0.8f;

// A ball that does not drop is treated as falling partway into the cup and
// meeting its far wall. How deep it gets is set by pace: a slow ball sinks in
// far enough to hit the wall square and is thrown back out, while a fast one
// is still crossing the mouth when it reaches the far side and only clips the
// rim. At CUP_SKIM_SPEED_PXPS it never drops at all and sails straight over.
constexpr float CUP_SKIM_SPEED_PXPS  = 1200.0f;
// Where the far wall sits, as a fraction of the cup radius.
constexpr float CUP_LIP_RADIUS_FRAC  = 0.62f;
// The wall only exists once the ball has fallen far enough to meet it; above
// this pace it is still skimming the surface and crosses with only a nudge.
constexpr float CUP_WALL_MIN_DIP     = 0.45f;
// After being thrown off the wall the ball is riding the rim rather than
// sitting in the cup. For this long nothing acts on it — no capture and, just
// as importantly, no pull, or the cup would simply reel it straight back in.
constexpr float CUP_REJECT_SECS      = 0.28f;
// Wall bounce: how much of the speed into the wall comes back, and how much
// of the speed along it survives the scrape.
constexpr float CUP_RIM_RESTITUTION  = 0.50f;
constexpr float CUP_RIM_TANGENT_KEEP = 0.75f;
// While riding the bowl the ball is pulled toward the middle and scrubbed by
// the rim. Both scale with how deep it has fallen.
constexpr float CUP_LIP_PULL_PXPS2 = 2400.0f;   // accel toward cup centre
constexpr float CUP_LIP_DRAG       = 2.0f;      // velocity bleed, 1/s

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
    // Must follow setPixelsPerMeter — the threshold is given in pixels.
    m_world->setRestitutionThresholdPx(WALL_BOUNCE_MIN_SPEED_PXPS);

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
    const float wallThick = COURSE_WALL_THICKNESS;

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

    // ---- Cup ----
    // No body: a sensor fires the instant the ball's circle grazes it, which
    // is what made every touch an instant hole-out, and it cannot express
    // "too fast to drop". updateCupInteraction() tests the geometry directly.

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
            // No Box2D damping: applyRollingFriction() decelerates instead.
            { 1.0f, 0.3f, 0.4f }, /*linearDamping*/ 0.0f);

        m_players[i].finishedHole[m_currentHole] = false;
        m_players[i].holedOut[m_currentHole]     = false;
        m_players[i].rotationRadians             = 0.0f;
        m_players[i].cupBounced                  = false;
        m_players[i].cupRejectTimer              = 0.0f;
        m_players[i].trail.clear();
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

    // 1a) Rolling friction, then the cup, so the cup reads settled velocities.
    applyRollingFriction(deltaTime);

    // 1b) Cup test runs in every phase, not just BallInMotion: the world is
    //     stepped throughout, so a ball still creeping toward the hole during
    //     a banner has to be able to drop.
    updateCupInteraction(deltaTime);

    // Sample where each moving ball is, for the trail behind it.
    m_trailSampleTimer += deltaTime;
    if(m_trailSampleTimer >= TRAIL_SAMPLE_SECS && m_world)
    {
        m_trailSampleTimer = 0.0f;
        for(auto& p : m_players)
        {
            if(!b2Body_IsValid(p.ballBody)) continue;
            if(getBodySpeedPx(*m_world, p.ballBody) <= 0.0f) continue;
            float tx = 0.0f, ty = 0.0f;
            getBodyPositionPx(*m_world, p.ballBody, tx, ty);
            p.trail.push_back({ tx, ty });
            if(p.trail.size() > TRAIL_MAX_POINTS) p.trail.erase(p.trail.begin());
        }
    }

    // 1c) Animation timers run in every phase. m_phaseTimer only ticks during
    //     the two banner phases, so it cannot drive anything that has to keep
    //     moving while the game waits for a dart.
    m_animClock += deltaTime;
    if(m_aimArrowTimer > 0.0f)
    {
        m_aimArrowTimer = std::max(0.0f, m_aimArrowTimer - deltaTime);
    }

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

            if(m_throwsRemainingInTurn == 0)
            {
                // Nothing left to throw but the turn was never closed out.
                // Draining here (as this used to) parks the game in Aiming
                // forever and silently eats every dart, so close the turn
                // instead — endCurrentTurn() either hands over or ends the
                // hole, both of which are recoverable states.
                endCurrentTurn();
                break;
            }

            DartPosition pos;
            if(popDartPosition(pos))
            {
                processDart(pos);
                // Drain extras: only one stroke per Aiming entry. The
                // settle / cup-capture path will return us to Aiming
                // for the next throw of this turn.
                DartPosition extra;
                while(popDartPosition(extra)) {}
            }
            break;
        }

        case Phase::BallInMotion:
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
    if(p.holedOut[m_currentHole])
    {
        // Already in the cup — nothing to hit. Returning bare here consumed
        // the dart without ending the turn, which left the game stuck in
        // Aiming. Close the turn out instead.
        p.finishedHole[m_currentHole] = true;
        endCurrentTurn();
        return;
    }

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

    // One trace on screen at a time — a stroke starts a clean picture.
    clearAllTrails();

    p.strokes[m_currentHole] = static_cast<uint8_t>(p.strokes[m_currentHole] + 1);
    if(m_throwsRemainingInTurn > 0) m_throwsRemainingInTurn--;
    m_phase         = Phase::BallInMotion;
    m_settleTimer   = 0.0f;
    m_lastShotHoled = false;
}


void MiniGolfGame::applyRollingFriction(float deltaTime)
{
    if(!m_world) return;

    const float drop = BALL_ROLL_DECEL_PXPS2 * deltaTime;

    for(auto& p : m_players)
    {
        if(!b2Body_IsValid(p.ballBody)) continue;

        float vx = 0.0f, vy = 0.0f;
        getBodyVelocityPx(*m_world, p.ballBody, vx, vy);
        const float speed = std::sqrt(vx * vx + vy * vy);

        // Already stopped. Skipping rather than writing a zero matters: setting
        // velocity on a sleeping body would wake it every frame.
        if(speed <= 0.0f) continue;

        const float next = speed - drop;
        if(next <= 0.0f)
        {
            // A real ball stops; it does not creep forever. This exact zero is
            // the whole point of the constant-deceleration model.
            setBodyVelocityPx(*m_world, p.ballBody, 0.0f, 0.0f);
        }
        else
        {
            const float scale = next / speed;
            setBodyVelocityPx(*m_world, p.ballBody, vx * scale, vy * scale);
        }
    }
}


void MiniGolfGame::holeOutPlayer(uint8_t playerIdx)
{
    if(playerIdx >= m_players.size()) return;
    if(m_currentHole >= HOLES_PER_GAME) return;

    PlayerState& p = m_players[playerIdx];
    if(p.holedOut[m_currentHole]) return;

    p.holedOut[m_currentHole]     = true;
    // In the cup means done with the hole, including when someone else's
    // shot knocked the ball in.
    p.finishedHole[m_currentHole] = true;

    // Destroy the body rather than freeze it. freezeBody() only zeroes the
    // velocity, so the ball stayed in the world as a fully collidable dynamic
    // circle while renderBalls() stopped drawing it — an invisible obstacle
    // parked on the cup that later balls bounced off.
    if(b2Body_IsValid(p.ballBody))
    {
        b2DestroyBody(p.ballBody);
        p.ballBody = b2_nullBodyId;
    }

    if(playerIdx == m_currentPlayer)
    {
        m_lastShotHoled = true;
    }
}


void MiniGolfGame::updateCupInteraction(float deltaTime)
{
    if(!m_world) return;
    if(m_currentHole >= HOLES_PER_GAME) return;

    const CourseHole& h = m_course.holes[m_currentHole];

    for(uint8_t i = 0; i < m_players.size(); ++i)
    {
        PlayerState& p = m_players[i];
        if(p.holedOut[m_currentHole]) continue;
        if(!b2Body_IsValid(p.ballBody)) continue;

        float bx = 0.0f, by = 0.0f;
        getBodyPositionPx(*m_world, p.ballBody, bx, by);
        const float dx = bx - h.cupPos.x;
        const float dy = by - h.cupPos.y;
        const float d  = std::sqrt(dx * dx + dy * dy);

        // The cup starts working on the ball as soon as the two overlap at
        // all — that is cupRadius + ball radius between centres, not
        // cupRadius. Testing the mouth alone left a whole ball-radius band
        // where the ball visibly hung over the hole and nothing touched it.
        // Dropping in still needs the ball's *centre* over the mouth; this
        // wider radius only governs where the lip starts to bite.
        const float influenceRadius = h.cupRadius + BALL_RADIUS_PX;
        if(d > influenceRadius)
        {
            p.cupBounced     = false;
            p.cupRejectTimer = 0.0f;
            continue;
        }

        // Just thrown off the far wall: the ball has been kicked up onto the
        // rim and is on its way out. Nothing acts on it until that expires —
        // no capture, and no pull either. The pull is a 1400 px/s^2 central
        // attractor, easily strong enough to arrest a rejected ball and drag
        // it back down the hole, which is what stopped lip-outs happening.
        if(p.cupRejectTimer > 0.0f)
        {
            p.cupRejectTimer -= deltaTime;
            continue;
        }

        const float speed = getBodySpeedPx(*m_world, p.ballBody);

        // ---- 1) Does it drop? -------------------------------------------
        const float speedFrac  = std::clamp(speed / CUP_CAPTURE_MAX_SPEED_PXPS,
                                            0.0f, 1.0f);
        const float dropRadius = h.cupRadius
                               * (1.0f - CUP_CAPTURE_RADIUS_FALLOFF * speedFrac);
        if(speed <= CUP_CAPTURE_MAX_SPEED_PXPS && d <= dropRadius)
        {
            holeOutPlayer(i);
            continue;
        }

        // ---- 2) How far into the cup does it fall? -----------------------
        // Everything below scales with this. At full pace the ball stays on
        // the surface and crosses untouched.
        const float dip = 1.0f - std::clamp(speed / CUP_SKIM_SPEED_PXPS,
                                            0.0f, 1.0f);
        if(dip <= 0.0f) continue;

        // Dead centre: no radial direction to work with, and nothing to hit.
        if(d <= 0.001f) continue;
        const float nx = dx / d;          // outward radial unit vector
        const float ny = dy / d;

        float vx = 0.0f, vy = 0.0f;
        getBodyVelocityPx(*m_world, p.ballBody, vx, vy);
        const float vOut = vx * nx + vy * ny;   // outward radial speed

        // ---- 3) The far wall of the cup ----------------------------------
        // This is what a ball struck too hard down the middle hits. The pull
        // in step 4 is radial, so for a ball crossing dead centre it has no
        // sideways component at all and cannot turn it — only the wall can.
        // The far wall is inside the cup, so it only applies once the ball's
        // centre is actually over the mouth — out in the overlap band there
        // is nothing to hit, only lip to ride.
        const float lipRadius = h.cupRadius * CUP_LIP_RADIUS_FRAC;
        if(!p.cupBounced && dip >= CUP_WALL_MIN_DIP
           && d >= lipRadius && d <= h.cupRadius && vOut > 0.0f)
        {
            // Not enough pace to climb back over the rim: it strikes the wall
            // and is thrown back across the hole. Speed into the wall comes
            // back reduced; speed along it is scrubbed by the scrape.
            const float tx = vx - vOut * nx;
            const float ty = vy - vOut * ny;
            setBodyVelocityPx(*m_world, p.ballBody,
                tx * CUP_RIM_TANGENT_KEEP - nx * vOut * CUP_RIM_RESTITUTION,
                ty * CUP_RIM_TANGENT_KEEP - ny * vOut * CUP_RIM_RESTITUTION);
            p.cupBounced     = true;
            p.cupRejectTimer = CUP_REJECT_SECS;
            continue;   // the bounce is this frame's cup interaction
        }

        // ---- 4) Riding the lip -------------------------------------------
        // Ramped so the bite fades to nothing at the edge of the overlap band
        // instead of switching on: full strength once the centre is over the
        // mouth, zero where the ball is only just touching the rim.
        const float lipFactor = std::clamp(
            (influenceRadius - d) / (influenceRadius - h.cupRadius), 0.0f, 1.0f);

        applyImpulsePxPerSec(*m_world, p.ballBody,
                             -nx * CUP_LIP_PULL_PXPS2 * dip * lipFactor * deltaTime,
                             -ny * CUP_LIP_PULL_PXPS2 * dip * lipFactor * deltaTime);
        applyImpulsePxPerSec(*m_world, p.ballBody,
                             -vx * CUP_LIP_DRAG * dip * lipFactor * deltaTime,
                             -vy * CUP_LIP_DRAG * dip * lipFactor * deltaTime);
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
    const bool holeDone = allPlayersFinishedHole();
    if(holeDone)
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
        // A finished hole is not handed straight to advanceToNextHole():
        // that skipped the "Hole N complete" banner entirely whenever the
        // board happened to be clear, which in the sim is always. Let the
        // HoleTransition phase run its own timer instead.
        if(!holeDone) beginNextTurn();
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
    if(m_throwsRemainingInTurn == 0)
    {
        // Out of strokes for this hole. Handing them an Aiming phase with
        // nothing to throw wedges the game, so retire them and move on.
        // Each pass through here marks one more player finished, so the
        // mutual recursion with endCurrentTurn() is bounded by the roster.
        m_players[m_currentPlayer].finishedHole[m_currentHole] = true;
        endCurrentTurn();
        return;
    }
    m_phase = Phase::Aiming;
}


void MiniGolfGame::advancePlayerWithinHole()
{
    const uint8_t n = static_cast<uint8_t>(m_players.size());
    if(n == 0) return;   // guard the modulo below
    for(uint8_t step = 1; step <= n; ++step)
    {
        uint8_t cand = (m_currentPlayer + step) % n;
        if(!m_players[cand].finishedHole[m_currentHole])
        {
            if(cand != m_currentPlayer)
            {
                // Hand-over: the outgoing player's path would otherwise sit on
                // the course through the whole of the next player's aim.
                clearAllTrails();
            }
            m_currentPlayer = cand;
            return;
        }
    }
}


void MiniGolfGame::clearAllTrails()
{
    for(auto& p : m_players) p.trail.clear();
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
        const GameOverAction action = handleGameOverKey(keycode, m_gameOverCursor);
        if(action == GameOverAction::Restart)       restartCurrentGame();
        else if(action == GameOverAction::MainMenu) loadGame(std::make_shared<MainMenu>());
        return;
    }
    (void)keycode;
}


void MiniGolfGame::onGamepadButton(uint8_t button, bool pressed)
{
    if(!pressed) return;
    if(m_phase == Phase::GameOver)
    {
        const GameOverAction action = handleGameOverGamepad(button, m_gameOverCursor);
        if(action == GameOverAction::Restart)       restartCurrentGame();
        else if(action == GameOverAction::MainMenu) loadGame(std::make_shared<MainMenu>());
        return;
    }
    (void)button;
}


void MiniGolfGame::onMissedThrow()
{
    // Bounced out / not detected: count one stroke without moving the ball.
    // Only meaningful while we are actually waiting on this player's dart.
    if(m_phase != Phase::Aiming) return;
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
    renderBallTrails();
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
    const float wt   = COURSE_WALL_THICKNESS;
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


void MiniGolfGame::renderBallTrails()
{
    FrameID fid = getFrameId();

    for(uint8_t i = 0; i < m_players.size(); ++i)
    {
        const PlayerState& p = m_players[i];
        if(p.trail.size() < 2) continue;

        const float last = static_cast<float>(p.trail.size() - 1);
        for(size_t j = 0; j < p.trail.size(); ++j)
        {
            // 0 at the oldest sample still held, 1 at the newest.
            const float age = static_cast<float>(j) / last;

            float sx = 0.0f, sy = 0.0f;
            m_camera.worldToScreen(p.trail[j].x, p.trail[j].y, sx, sy);

            auto dot = std::make_shared<RenderShape>();
            dot->m_type   = ShapeType::Circle;
            dot->m_color  = lerpColor(FELT_COLOR, p.ballColor,
                                      TRAIL_FADE_TAIL
                                      + (TRAIL_FADE_HEAD - TRAIL_FADE_TAIL) * age);
            dot->m_x      = sx;
            dot->m_y      = sy;
            dot->m_z      = Z_TRAIL;
            dot->m_width  = 2.0f * m_camera.worldToScreenLength(2.0f + 4.0f * age);
            dot->m_height = 0.0f;
            renderQueueAdd(fid, dot);
        }
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
            // m_animClock is the only timer that advances during Aiming;
            // the previous m_phaseTimer + m_aimArrowTimer expression was
            // constant there, so the "pulse" never actually moved.
            pulseScale = 1.0f + 0.06f * std::sin(m_animClock * 6.0f);
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
        // Detail: strokes on the current hole. advanceToNextHole() leaves
        // m_currentHole == HOLES_PER_GAME once the round is over, so the
        // per-hole line is only valid while a hole is actually in play.
        if(m_currentHole < HOLES_PER_GAME)
        {
            const uint8_t s = p.strokes[m_currentHole];
            e.detailText = "Hole " + std::to_string(m_currentHole + 1)
                         + ": " + std::to_string(s);
        }
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
