/**
 * x01.cpp
 *
 * X01 game implementation.
 */

#include "x01.hpp"
#include "dart/dart_defs.hpp"
#include "frame/render_queue.hpp"
#include "game_lib/game_manager.hpp"
#include "game_lib/game_helpers.hpp"
#include "game_lib/components/render_shape.hpp"
#include "game_lib/components/render_text.hpp"
#include "games/main_menu.hpp"
#include "players/players.hpp"

#include <cstdio>
#include <SDL3/SDL_keycode.h>
#include <SDL3/SDL_gamepad.h>
#include <SDL3_ttf/SDL_ttf.h>


static constexpr float  FONT_SIZE       = 20.0f;
static constexpr float  LARGE_FONT_SIZE = 64.0f;

// Left panel — current player detail
static constexpr float LEFT_PANEL_X      = 10.0f;
static constexpr float LEFT_NAME_Y       = 120.0f;
static constexpr float LEFT_SCORE_Y      = 170.0f;
static constexpr float LEFT_PROGRESS_Y   = 280.0f;
static constexpr float LEFT_DARTS_Y      = 320.0f;
static constexpr float LEFT_DART_ROW_H   = 28.0f;

static constexpr float BUST_DISPLAY_TIME = 2.0f; // seconds to show BUST indicator

/** Check whether a dart segment satisfies an in/out rule. */
static bool satisfiesRule(X01InOutRule rule, DartSegment segment)
{
    if(rule == X01InOutRule::Any) return true;
    DartRing ring = getSegmentRing(segment);
    if(ring == DartRing::Double || ring == DartRing::InnerBull) return true;
    if(rule == X01InOutRule::Master && ring == DartRing::Triple) return true;
    return false;
}


X01Game::X01Game(X01Variant variant, X01InOutRule outRule, X01InOutRule inRule,
                 uint8_t legsToWin, X01StartingPlayer startingPlayer)
    : Game("X01")
    , m_variant(variant)
    , m_outRule(outRule)
    , m_inRule(inRule)
    , m_legsToWin(legsToWin)
    , m_startingPlayer(startingPlayer)
    , m_fontId(INVALID_FONT_ID)
    , m_largeFontId(INVALID_FONT_ID)
    , m_board()
{
}


Status X01Game::init(FrameID frameId)
{
    m_frameId = frameId;

    // Load fonts for board labels and score display
    m_fontId = loadFont("assets/fonts/Roboto-Regular.ttf", FONT_SIZE);
    if(m_fontId == INVALID_FONT_ID)
    {
        return STATUS_ERROR_GENERIC;
    }

    m_largeFontId = loadFont("assets/fonts/Roboto-Regular.ttf", LARGE_FONT_SIZE);
    if(m_largeFontId == INVALID_FONT_ID)
    {
        return STATUS_ERROR_GENERIC;
    }

    // Create the dart board centered in the window
    m_board = DartBoard::create(m_world, GameLayout::BOARD_CENTER_X, GameLayout::BOARD_CENTER_Y,
                                GameLayout::BOARD_SCALE, m_fontId);

    // Initialize per-player scores
    uint16_t startScore = static_cast<uint16_t>(m_variant);
    m_playerScores.assign(getPlayerCount(), startScore);
    m_turnStartScore = startScore;
    m_turnScoreProgression.clear();

    // Initialize in-rule tracking
    bool startedByDefault = (m_inRule == X01InOutRule::Any);
    m_playerStarted.assign(getPlayerCount(), startedByDefault);

    // Initialize PPR tracking
    m_playerTotalPoints.assign(getPlayerCount(), 0);
    m_playerTotalDarts.assign(getPlayerCount(), 0);

    // Initialize leg tracking
    m_playerLegs.assign(getPlayerCount(), 0);
    m_legStartPlayer = 0;
    m_currentLeg = 1;

    return STATUS_OK;
}


void X01Game::update(float deltaTime)
{
    // Game over — stop processing darts (pause is handled by GameManager)
    if(m_gameOver)
    {
        return;
    }

    // Bust display timer
    if(m_showBust)
    {
        m_bustTimer -= deltaTime;
        if(m_bustTimer <= 0.0f)
        {
            m_showBust = false;
        }
    }

    uint16_t& score = m_playerScores[m_currentPlayerIndex];

    // Blink highlighted segments
    if(!m_hitSegments.empty() && updateBlink(deltaTime, m_blinkTimer, m_blinkOn))
    {
        for(auto seg : m_hitSegments)
        {
            if(m_blinkOn) m_board.highlightSegment(seg);
            else          m_board.unhighlightSegment(seg);
        }
    }

    // Drain dart events — always consume so queues don't back up
    uint32_t landed = consumeDartLandedCount();

    // Show "Processing..." only if a dart landed but no position data is available yet
    DartPosition pos;
    bool hasPosition = popDartPosition(pos);
    if(landed > 0 && !hasPosition && m_throwsRemaining > 0)
    {
        LOG_INFO(GAME_MANAGER_LOG_ID, "X01: Dart landed, waiting for position...");
        m_statusText = "Hold";
    }

    // Process first position (already popped above) and any remaining
    while(hasPosition)
    {
        if(m_throwsRemaining == 0)
        {
            // Extra throws beyond 3 are ignored
            hasPosition = popDartPosition(pos);
            continue;
        }

        auto segment = polarToSegment(pos.angle, pos.normalizedRadius);
        if(!segment.has_value())
        {
            LOG_WARNING(GAME_MANAGER_LOG_ID, "X01: Received off-board position");
            m_statusText = "Waiting for Throw";
            hasPosition = popDartPosition(pos);
            continue;
        }

        uint16_t points = getSegmentPoints(segment.value());
        LOG_INFO(GAME_MANAGER_LOG_ID, "X01: Hit segment {} for {} points",
                 static_cast<int>(segment.value()), points);

        m_throwsRemaining--;
        m_statusText = "Waiting for Throw";

        // In Rule: if the player hasn't started yet, only a qualifying dart counts
        if(!m_playerStarted[m_currentPlayerIndex])
        {
            if(satisfiesRule(m_inRule, segment.value()))
            {
                m_playerStarted[m_currentPlayerIndex] = true;
                LOG_INFO(GAME_MANAGER_LOG_ID, "X01: Player {} satisfied in rule", m_currentPlayerIndex);
            }
            else
            {
                // Dart doesn't count — still track segment for visual feedback
                m_hitSegments.push_back(segment.value());
                m_hitPositions.push_back(pos);
                m_board.highlightSegment(segment.value());
                m_turnScoreProgression.push_back(score); // score unchanged
                if(m_throwsRemaining == 0) m_waitingForCollect = true;
                hasPosition = popDartPosition(pos);
                continue;
            }
        }

        // Check for bust conditions
        bool bust = false;
        uint16_t newScore = 0;

        if(points > score)
        {
            bust = true; // Would go negative
        }
        else
        {
            newScore = score - points;
        }

        if(!bust && newScore == 0)
        {
            // Reached zero — check out rule
            if(!satisfiesRule(m_outRule, segment.value()))
            {
                bust = true; // Didn't finish on required double/master
            }
        }

        if(!bust && newScore == 1 && m_outRule == X01InOutRule::Double)
        {
            bust = true; // Can't finish on a double from 1
        }

        if(bust)
        {
            LOG_INFO(GAME_MANAGER_LOG_ID, "X01: Player {} busts!", m_currentPlayerIndex);
            score = m_turnStartScore;
            m_throwsRemaining = 0;
            m_showBust = true;
            m_bustTimer = BUST_DISPLAY_TIME;
            m_waitingForCollect = true;

            // Still highlight the segment that caused the bust
            m_hitSegments.push_back(segment.value());
            m_hitPositions.push_back(pos);
            m_board.highlightSegment(segment.value());
            break;
        }

        // Apply score
        score = newScore;

        // Check for checkout — wait for dart collection before transitioning
        if(score == 0)
        {
            // PPR: checkout — count only darts actually thrown this visit
            uint16_t dartsThisVisit = 3 - m_throwsRemaining;
            m_playerTotalPoints[m_currentPlayerIndex] += m_turnStartScore;
            m_playerTotalDarts[m_currentPlayerIndex] += dartsThisVisit;

            m_throwsRemaining = 0;
            m_waitingForCollect = true;
            m_statusText = "Collect Darts";

            // Pre-increment leg count so render can check match vs leg win
            m_playerLegs[m_currentPlayerIndex]++;
            m_legWinnerIndex = m_currentPlayerIndex;
            break;
        }

        // Track turn progression
        m_turnScoreProgression.push_back(score);

        // Track and highlight the hit segment
        m_hitSegments.push_back(segment.value());
        m_hitPositions.push_back(pos);
        m_board.highlightSegment(segment.value());

        // All throws used — wait for board to be cleared
        if(m_throwsRemaining == 0)
        {
            m_waitingForCollect = true;
        }

        hasPosition = popDartPosition(pos);
    }

    // When waiting for darts to be collected, poll the vision source
    if(m_waitingForCollect && isBoardClear())
    {
        LOG_INFO(GAME_MANAGER_LOG_ID, "X01: Board cleared");

        m_board.unhighlightAll();
        m_hitSegments.clear();
        m_hitPositions.clear();
        m_blinkTimer = 0.0f;
        m_blinkOn = true;
        m_waitingForCollect = false;

        // Check if current player checked out (score == 0)
        // Leg count was already incremented at checkout time
        if(m_playerScores[m_currentPlayerIndex] == 0)
        {
            if(m_playerLegs[m_currentPlayerIndex] >= m_legsToWin)
            {
                // Match or single-leg game over
                m_gameOver = true;
                m_winnerIndex = m_currentPlayerIndex;
                m_gameOverCursor = 0;
                LOG_INFO(GAME_MANAGER_LOG_ID, "X01: Player {} wins!", m_currentPlayerIndex);
                return;
            }

            // Start a new leg
            m_currentLeg++;

            // Determine who starts the next leg
            uint8_t nextStarter = 0;
            switch(m_startingPlayer)
            {
                case X01StartingPlayer::Rotate:
                    nextStarter = (m_legStartPlayer + 1) % getPlayerCount();
                    break;
                case X01StartingPlayer::Winner:
                    nextStarter = m_currentPlayerIndex;
                    break;
                case X01StartingPlayer::Loser:
                    nextStarter = (m_currentPlayerIndex + 1) % getPlayerCount();
                    break;
            }
            m_legStartPlayer = nextStarter;
            m_currentPlayerIndex = nextStarter;

            // Reset scores and in-rule tracking
            uint16_t startScore = static_cast<uint16_t>(m_variant);
            m_playerScores.assign(getPlayerCount(), startScore);
            bool startedByDefault = (m_inRule == X01InOutRule::Any);
            m_playerStarted.assign(getPlayerCount(), startedByDefault);

            // Reset turn state
            m_throwsRemaining = 3;
            m_turnScoreProgression.clear();
            m_turnStartScore = startScore;
            m_showBust = false;
            m_statusText = "Waiting for Throw";
        }
        else
        {
            // PPR: normal visit end — always 3 darts, points = what was actually scored
            // (0 on bust since score reverted to m_turnStartScore)
            uint16_t pointsThisVisit = m_turnStartScore - m_playerScores[m_currentPlayerIndex];
            m_playerTotalPoints[m_currentPlayerIndex] += pointsThisVisit;
            m_playerTotalDarts[m_currentPlayerIndex] += 3;

            // Advance to next player
            m_currentPlayerIndex = (m_currentPlayerIndex + 1) % getPlayerCount();
            m_throwsRemaining = 3;
            m_turnScoreProgression.clear();
            m_turnStartScore = m_playerScores[m_currentPlayerIndex];
            m_statusText = "Waiting for Throw";
        }
    }
}


void X01Game::render()
{
    // Enqueue the dart board (GameManager handles clear/flush/present)
    m_board.enqueueRender(getFrameId());
    renderHitMarkers(getFrameId(), m_hitPositions,
                     GameLayout::BOARD_CENTER_X, GameLayout::BOARD_CENTER_Y,
                     GameLayout::BOARD_SCALE);
    renderRightScoreboard();
    renderLeftPlayerDetail();

    if(m_gameOver)
    {
        renderGameOver();
    }
}


void X01Game::renderRightScoreboard()
{
    uint8_t playerCount = getPlayerCount();
    std::vector<ScoreboardEntry> entries;
    entries.reserve(playerCount);

    for(uint8_t i = 0; i < playerCount; i++)
    {
        PlayerID pid = getPlayerByIndex(i);
        ScoreboardEntry entry;
        entry.name  = getPlayerName(pid);
        entry.value = std::to_string(m_playerScores[i]);

        // Build detail line with legs and PPR
        std::string detail;
        if(m_legsToWin > 1)
        {
            detail += "Legs: " + std::to_string(m_playerLegs[i])
                    + "/" + std::to_string(m_legsToWin);
        }
        if(m_playerTotalDarts[i] > 0)
        {
            float ppr = (static_cast<float>(m_playerTotalPoints[i]) / m_playerTotalDarts[i]) * 3.0f;
            char buf[16];
            snprintf(buf, sizeof(buf), "%.1f", ppr);
            if(!detail.empty()) detail += "   ";
            detail += std::string("PPR: ") + buf;
        }
        else
        {
            if(!detail.empty()) detail += "   ";
            detail += "PPR: -";
        }
        entry.detailText = detail;

        entries.push_back(entry);
    }

    renderScoreboardPanel(getFrameId(), m_fontId, entries, m_currentPlayerIndex);
}


void X01Game::renderLeftPlayerDetail()
{
    FrameID fid = getFrameId();
    uint16_t score = m_playerScores[m_currentPlayerIndex];
    PlayerID pid = getPlayerByIndex(m_currentPlayerIndex);
    std::string name = getPlayerName(pid);

    // Match info (multi-leg only)
    if(m_legsToWin > 1)
    {
        std::string legInfo = "Leg " + std::to_string(m_currentLeg)
                            + "  |  First to " + std::to_string(m_legsToWin);
        auto legText = std::make_shared<RenderText>();
        legText->m_text     = legInfo;
        legText->m_color    = {160, 200, 255};
        legText->m_fontId   = m_fontId;
        legText->m_rotation = 0.0f;
        legText->m_scaleX   = 1.0f;
        legText->m_scaleY   = 1.0f;
        legText->m_x        = LEFT_PANEL_X;
        legText->m_y        = LEFT_NAME_Y - 30.0f;
        legText->m_z        = GameLayout::SIDEBAR_Z;
        renderQueueAdd(fid, legText);
    }

    // Current player name
    auto nameText = std::make_shared<RenderText>();
    nameText->m_text     = name;
    nameText->m_color    = {200, 200, 200};
    nameText->m_fontId   = m_largeFontId;
    nameText->m_rotation = 0.0f;
    nameText->m_scaleX   = 0.6f;
    nameText->m_scaleY   = 0.6f;
    nameText->m_x        = LEFT_PANEL_X;
    nameText->m_y        = LEFT_NAME_Y;
    nameText->m_z        = GameLayout::SIDEBAR_Z;
    renderQueueAdd(fid, nameText);

    // Current score (large, native 48pt for crisp rendering)
    auto scoreText = std::make_shared<RenderText>();
    scoreText->m_text     = std::to_string(score);
    scoreText->m_color    = {255, 255, 255};
    scoreText->m_fontId   = m_largeFontId;
    scoreText->m_rotation = 0.0f;
    scoreText->m_scaleX   = 1.0f;
    scoreText->m_scaleY   = 1.0f;
    scoreText->m_x        = LEFT_PANEL_X;
    scoreText->m_y        = LEFT_SCORE_Y;
    scoreText->m_z        = GameLayout::SIDEBAR_Z;
    renderQueueAdd(fid, scoreText);

    // BUST indicator (large red text overlaying the score area)
    if(m_showBust)
    {
        auto bustText = std::make_shared<RenderText>();
        bustText->m_text     = "BUST";
        bustText->m_color    = {220, 40, 40};
        bustText->m_fontId   = m_largeFontId;
        bustText->m_rotation = 0.0f;
        bustText->m_scaleX   = 1.0f;
        bustText->m_scaleY   = 1.0f;
        bustText->m_x        = LEFT_PANEL_X + 120.0f;
        bustText->m_y        = LEFT_SCORE_Y;
        bustText->m_z        = GameLayout::SIDEBAR_Z + 1;
        renderQueueAdd(fid, bustText);
    }

    // Checkout banner — show only while waiting for dart collection
    if(m_waitingForCollect && score == 0)
    {
        PlayerID legWinnerId = getPlayerByIndex(m_legWinnerIndex);
        std::string winnerName = getPlayerName(legWinnerId);
        bool isMatchWin = (m_playerLegs[m_legWinnerIndex] >= m_legsToWin);
        std::string bannerMsg = isMatchWin ? "GAME!" : "LEG!";
        renderAnnouncementBanner(fid, m_largeFontId, winnerName, bannerMsg);
    }

    // In-rule indicator: show what the player needs before darts count
    if(m_inRule != X01InOutRule::Any && !m_playerStarted[m_currentPlayerIndex])
    {
        std::string needsText = (m_inRule == X01InOutRule::Double)
            ? "Needs Double" : "Needs Double/Triple";
        auto inText = std::make_shared<RenderText>();
        inText->m_text     = needsText;
        inText->m_color    = {255, 180, 60};
        inText->m_fontId   = m_fontId;
        inText->m_rotation = 0.0f;
        inText->m_scaleX   = 1.0f;
        inText->m_scaleY   = 1.0f;
        inText->m_x        = LEFT_PANEL_X;
        inText->m_y        = LEFT_SCORE_Y + 70.0f;
        inText->m_z        = GameLayout::SIDEBAR_Z;
        renderQueueAdd(fid, inText);
    }

    // Turn progression: "501 > 481 > 461"
    std::string progression = std::to_string(m_turnStartScore);
    for(auto s : m_turnScoreProgression)
    {
        progression += " > " + std::to_string(s);
    }

    auto progText = std::make_shared<RenderText>();
    progText->m_text     = progression;
    progText->m_color    = {180, 180, 180};
    progText->m_fontId   = m_fontId;
    progText->m_rotation = 0.0f;
    progText->m_scaleX   = 1.0f;
    progText->m_scaleY   = 1.0f;
    progText->m_x        = LEFT_PANEL_X;
    progText->m_y        = LEFT_PROGRESS_Y;
    progText->m_z        = GameLayout::SIDEBAR_Z;
    renderQueueAdd(fid, progText);

    // Per-dart point values
    uint16_t prev = m_turnStartScore;
    for(size_t i = 0; i < m_turnScoreProgression.size(); i++)
    {
        uint16_t dartPoints = prev - m_turnScoreProgression[i];
        prev = m_turnScoreProgression[i];

        auto dartText = std::make_shared<RenderText>();
        dartText->m_text     = "D" + std::to_string(i + 1) + ": " + std::to_string(dartPoints);
        dartText->m_color    = {200, 200, 200};
        dartText->m_fontId   = m_fontId;
        dartText->m_rotation = 0.0f;
        dartText->m_scaleX   = 1.0f;
        dartText->m_scaleY   = 1.0f;
        dartText->m_x        = LEFT_PANEL_X;
        dartText->m_y        = LEFT_DARTS_Y + i * LEFT_DART_ROW_H;
        dartText->m_z        = GameLayout::SIDEBAR_Z;
        renderQueueAdd(fid, dartText);
    }

    // Turn total
    if(!m_turnScoreProgression.empty())
    {
        uint16_t turnTotal = m_turnStartScore - score;
        float totalY = LEFT_DARTS_Y + m_turnScoreProgression.size() * LEFT_DART_ROW_H + 8.0f;

        auto totalText = std::make_shared<RenderText>();
        totalText->m_text     = "Total: " + std::to_string(turnTotal);
        totalText->m_color    = {255, 220, 100};
        totalText->m_fontId   = m_fontId;
        totalText->m_rotation = 0.0f;
        totalText->m_scaleX   = 1.0f;
        totalText->m_scaleY   = 1.0f;
        totalText->m_x        = LEFT_PANEL_X;
        totalText->m_y        = totalY;
        totalText->m_z        = GameLayout::SIDEBAR_Z;
        renderQueueAdd(fid, totalText);
    }
}


void X01Game::shutdown()
{
    if(m_largeFontId != INVALID_FONT_ID)
    {
        unloadFont(m_largeFontId);
        m_largeFontId = INVALID_FONT_ID;
    }

    if(m_fontId != INVALID_FONT_ID)
    {
        unloadFont(m_fontId);
        m_fontId = INVALID_FONT_ID;
    }
}


GameBarInfo X01Game::getBarInfo() const
{
    return makeBarInfo(m_gameOver, m_waitingForCollect,
                       m_currentPlayerIndex, m_throwsRemaining, m_statusText);
}


uint8_t X01Game::getMaxPlayers() const
{
    return 6;
}


// ============================================================================
// Game-over input handling
// ============================================================================

void X01Game::onKeyDown(uint32_t keycode)
{
    if(!m_gameOver) return;

    auto action = handleGameOverKey(keycode, m_gameOverCursor);
    if(action == GameOverAction::Restart)       restartCurrentGame();
    else if(action == GameOverAction::MainMenu) loadGame(std::make_shared<MainMenu>());
}


void X01Game::onGamepadButton(uint8_t button, bool pressed)
{
    if(!pressed || !m_gameOver) return;

    auto action = handleGameOverGamepad(button, m_gameOverCursor);
    if(action == GameOverAction::Restart)       restartCurrentGame();
    else if(action == GameOverAction::MainMenu) loadGame(std::make_shared<MainMenu>());
}


void X01Game::renderGameOver()
{
    PlayerID winnerId = getPlayerByIndex(m_winnerIndex);
    renderGameOverOverlay(getFrameId(), m_largeFontId, m_fontId,
                          getPlayerName(winnerId), m_gameOverCursor);
}
