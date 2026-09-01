/**
 * minigolf.hpp
 *
 * Mini golf game mode. Each dart throw becomes one putt: the angle from
 * bullseye sets the direction, and the distance from bullseye sets the
 * power. Players cycle within a hole; whoever's turn it is sees their
 * ball pulse. Game ends after 9 holes; per-hole scores cap at STROKE_CAP
 * (8) strokes.
 *
 * Physics is Box2D v3 via the generic game_lib/box2d helpers. The world
 * is rebuilt per hole: walls, cup sensor, and one ball per player.
 */

#ifndef MINIGOLF_HPP
#define MINIGOLF_HPP

#include "game_lib/game.hpp"
#include "game_lib/box2d/physics_world.hpp"
#include "game_lib/box2d/physics_body.hpp"
#include "game_lib/box2d/physics_camera.hpp"
#include "course_defs.hpp"

#include "box2d/box2d.h"

#include <array>
#include <memory>
#include <vector>


namespace MiniGolf
{

enum class Phase : uint8_t
{
    HoleIntro,        // brief banner showing current hole #
    Aiming,           // active player's ball at rest, waiting for a dart
    BallInMotion,     // active player's ball is rolling
    HoleTransition,   // brief banner between holes ("Hole 3/9")
    GameOver
};


struct PlayerState
{
    Color    ballColor;
    uint8_t  strokes[HOLES_PER_GAME] = {};   // strokes used on each hole
    bool     finishedHole[HOLES_PER_GAME] = {};
    bool     holedOut[HOLES_PER_GAME] = {};

    // Per-hole transient state
    b2BodyId          ballBody = b2_nullBodyId;
    PhysicsUserData   ballUserData;
    float             rotationRadians = 0.0f;  // accumulator for visible spin

    // Where this ball has been since the current stroke was struck, oldest
    // first, in world-space pixels. Rendered as the fading trail behind it.
    std::vector<Vec2> trail;

    // Cup interaction, reset every time the ball leaves the cup mouth.
    // cupBounced allows one strike against the far wall per visit, so a
    // rejected ball cannot be batted back and forth forever.
    bool              cupBounced     = false;
    float             cupRejectTimer = 0.0f;

    uint16_t totalStrokes() const
    {
        uint16_t sum = 0;
        for(uint8_t s : strokes) sum += s;
        return sum;
    }
};


class MiniGolfGame : public Game
{
    public:
        explicit MiniGolfGame(CourseId courseId);
        ~MiniGolfGame() override = default;

        Status init(FrameID frameId) override;
        void   update(float deltaTime) override;
        void   render() override;
        void   shutdown() override;

        GameBarInfo getBarInfo() const override;
        uint8_t     getMaxPlayers() const override;

        void onKeyDown(uint32_t keycode) override;
        void onGamepadButton(uint8_t button, bool pressed) override;
        void onMissedThrow() override;

    private:
        // ── Hole lifecycle ─────────────────────────────────────────────
        void buildCurrentHole();
        void teardownCurrentHole();
        void resetBallsToStart();

        // ── Stroke flow ────────────────────────────────────────────────
        void processDart(const DartPosition& pos);
        void onBallSettled();
        void endCurrentTurn();
        void beginNextTurn();
        void advancePlayerWithinHole();
        void clearAllTrails();
        bool allPlayersFinishedHole() const;
        void advanceToNextHole();
        uint8_t throwsAvailableForPlayer(uint8_t playerIdx) const;

        // ── Rolling / cup / settling ───────────────────────────────────
        void applyRollingFriction(float deltaTime);
        void updateCupInteraction(float deltaTime);
        void holeOutPlayer(uint8_t playerIdx);
        void updateBallMotion(float deltaTime);

        // ── Render helpers ─────────────────────────────────────────────
        void renderBallTrails();
        void renderCourse();
        void renderHashCompass();
        void renderBalls();
        void renderAimArrow();
        void renderHoleBanner();
        void renderScorecardPanel();
        void renderGameOverScreen();

        // ── Members ────────────────────────────────────────────────────
        CourseId     m_courseId;
        Course       m_course;

        FontID       m_fontId      = INVALID_FONT_ID;
        FontID       m_largeFontId = INVALID_FONT_ID;

        std::unique_ptr<PhysicsWorld> m_world;
        PhysicsCamera                 m_camera;

        // Static bodies for the current hole (regenerated per hole).
        // The cup has no body of its own: it is a hole in the floor, tested
        // geometrically against cupPos/cupRadius by updateCupInteraction().
        std::vector<b2BodyId> m_wallBodies;

        std::vector<PlayerState> m_players;

        Phase   m_phase             = Phase::HoleIntro;
        float   m_phaseTimer        = 0.0f;
        uint8_t m_currentHole       = 0;
        uint8_t m_currentPlayer     = 0;
        // Each turn the active player gets up to 3 throws (or fewer if the
        // stroke cap would be exceeded). Counts down per stroke; turn
        // ends when this hits 0 or the ball goes in the cup.
        uint8_t m_throwsRemainingInTurn = 0;
        bool    m_waitingForCollect = false;
        // For edge-detecting dart-pull from the vision module.
        bool    m_lastBoardClear    = true;

        // Settle detection: ball is "stopped" once speed has stayed
        // below the threshold for this long.
        float   m_settleTimer       = 0.0f;
        bool    m_lastShotHoled     = false;

        // Paces how often ball positions are recorded for the trail.
        float   m_trailSampleTimer  = 0.0f;

        // Free-running clock for idle animation (the active-ball pulse).
        // Advanced every update regardless of phase, so the pulse keeps
        // moving while the game sits waiting for a dart.
        float   m_animClock         = 0.0f;

        // Aim arrow state — populated when a stroke is initiated, fades.
        float   m_aimArrowTimer     = 0.0f;
        float   m_aimArrowDirX      = 0.0f;
        float   m_aimArrowDirY      = 0.0f;
        float   m_aimArrowLengthPx  = 0.0f;
        float   m_aimArrowOriginX   = 0.0f;
        float   m_aimArrowOriginY   = 0.0f;

        uint8_t m_gameOverCursor = 0;
};

}  // namespace MiniGolf

#endif // MINIGOLF_HPP
