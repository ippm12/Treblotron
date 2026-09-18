/**
 * minigolf.hpp
 *
 * Mini golf game mode. Each dart throw becomes one putt: the angle from
 * bullseye sets the direction, and the distance from bullseye sets the
 * power. Players cycle within a hole. Game ends after 9 holes;
 * per-hole scores cap at STROKE_CAP
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
#include "course_io.hpp"
#include "ball_roll.hpp"
#include "course_motion.hpp"

#include "box2d/box2d.h"

#include <array>
#include <memory>
#include <vector>


namespace MiniGolf
{

enum class TeamMode : uint8_t { Individual, AlternateShot, Scramble };
struct GameOptions {
    bool ballCollisions=true;
    uint8_t holeCount=9;
    uint8_t startHole=0;
    TeamMode teams=TeamMode::Individual;
    std::string courseDirectory{};
};
inline Course selectedCourse(CourseId id,const GameOptions& options) {
    const auto source=options.courseDirectory.empty() ? buildCourse(id) : loadCourse(options.courseDirectory);
    auto result=source;
    for(size_t i=0;i<HOLES_PER_GAME;++i)
        result.holes[i]=source.holes[(options.startHole+i)%HOLES_PER_GAME];
    return result;
}

enum class Phase : uint8_t
{
    HoleIntro,        // brief banner showing current hole #
    Aiming,           // active player's ball at rest, waiting for a dart
    BallInMotion,     // active player's ball is rolling
    HoleTransition,   // brief banner between holes ("Hole 3/9")
    ScrambleChoice,
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
    BallRoll          roll;

    // Where this ball has been since the current stroke was struck, oldest
    // first, in world-space pixels. Rendered as the fading trail behind it.
    std::vector<Vec2> trail;

    // Cup interaction, reset every time the ball leaves the cup mouth.
    // cupBounced allows one strike against the far wall per visit, so a
    // rejected ball cannot be batted back and forth forever.
    bool              cupBounced     = false;
    float             cupRejectTimer = 0.0f;
    Vec2              shotStart;
    bool              hasSpawned = false;
    bool              pendingReturn = false;
    float             returnDelay = 0;
    bool              safeReturnRequired = false;
    float             destructionTimer = 0;
    Vec2              destructionPosition;
    float             hazardTimer = 0;
    float             hazardImmunity = 0;
    bool              respawnProtected = false;
    int               blockedPortalPair = -1;
    float             portalCooldown = 0;
    float             bumperCooldown = 0;
    float             crushTimer = 0;
    int               crushWallA = -1, crushWallB = -1;
    Vec2              hazardPosition;
    bool              waterSplash = false;

    uint16_t totalStrokes() const
    {
        uint16_t sum = 0;
        for(uint8_t s : strokes) sum += s;
        return sum;
    }
};


struct ScrambleAttempt {
    Vec2 position;
    uint8_t strokes=0;
    bool holed=false;
    uint8_t member=0;
};
struct TeamState {
    std::string name;
    std::vector<uint8_t> members;
    size_t cursor=0;
    size_t attemptsTaken=0;
    Vec2 origin;
    uint8_t baseStrokes=0;
    bool attemptStarted=false;
    std::vector<ScrambleAttempt> attempts;
};

class MiniGolfGame : public Game
{
    public:
        explicit MiniGolfGame(CourseId courseId, GameOptions options = {});
        ~MiniGolfGame() override = default;

        Status init(FrameID frameId) override;
        void   update(float deltaTime) override;
        void   render() override;
        void   shutdown() override;

        GameBarInfo getBarInfo() const override;
        std::vector<std::string> getPauseActions() const override;
        void onPauseAction(size_t index) override;
        uint8_t     getMaxPlayers() const override;

        void onKeyDown(uint32_t keycode) override;
        void onGamepadButton(uint8_t button, bool pressed) override;
        void onMissedThrow() override;
        void onMouseClick(float x,float y,uint8_t button) override;

    private:
        friend class CoursePreview;
        void stepCoursePhysics(float deltaTime);
        // ── Hole lifecycle ─────────────────────────────────────────────
        void buildCurrentHole();
        void spawnBall(size_t player, Vec2 position);
        void returnDisplacedBalls(float dt);
        bool findSafeReturn(size_t player, Vec2 origin, Vec2& result) const;
        bool ballPositionOccupied(Vec2 position, size_t except) const;
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
        void buildObstacles();
        void moveObstacles(float dt);
        void updateObstacles(float dt);
        void renderObstacles(bool drawSurfaces=true,bool drawRails=true);
        void penalizeHazard(size_t player, Vec2 position, bool water);
        Vec2 obstaclePosition(Vec2 base, int rail) const;
        Vec2 cupPosition() const;

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
        void configureTeams(uint8_t playerCount);
        uint8_t activeMember() const;
        std::string competitorName(size_t competitor) const;
        void startTeamAttempt();
        bool finishTeamAttempt();
        void chooseScramble(size_t choice);
        void renderScramble();

        GameOptions  m_options;
        CourseId     m_courseId;
        Course       m_course;

        FontID       m_fontId      = INVALID_FONT_ID;
        FontID       m_largeFontId = INVALID_FONT_ID;
        FontID       m_compassFontId = INVALID_FONT_ID;

        std::unique_ptr<PhysicsWorld> m_world;
        PhysicsCamera                 m_camera;

        // Static bodies for the current hole (regenerated per hole).
        // The cup has no body of its own: it is a hole in the floor, tested
        // geometrically against cupPos/cupRadius by updateCupInteraction().
        std::vector<b2BodyId> m_wallBodies;
        std::vector<b2BodyId> m_bumperBodies;
        std::vector<float> m_bumperAnimation;
        double m_courseTime = 0;

        std::vector<PlayerState> m_players;
        std::vector<TeamState> m_teams;
        size_t m_scrambleChoice=0;
        bool m_committingScramble=false;

        Phase   m_phase             = Phase::HoleIntro;
        float   m_phaseTimer        = 0.0f;
        uint8_t m_currentHole       = 0;
        uint8_t m_cascadeParticipants = 0;
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
