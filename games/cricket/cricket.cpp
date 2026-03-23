/**
 * cricket.cpp
 *
 * Cricket game implementation.
 */

#include "cricket.hpp"
#include "dart/dart_defs.hpp"
#include "frame/render_queue.hpp"
#include "game_lib/game_manager.hpp"
#include "game_lib/game_helpers.hpp"
#include "vision/vision.hpp"
#include "game_lib/components/render_shape.hpp"
#include "game_lib/components/render_text.hpp"
#include "games/main_menu.hpp"
#include "players/players.hpp"

#include <SDL3/SDL_keycode.h>
#include <SDL3/SDL_gamepad.h>
#include <SDL3_ttf/SDL_ttf.h>

#include <algorithm>
#include <random>


static constexpr float  FONT_SIZE       = 30.0f;
static constexpr float  LARGE_FONT_SIZE = 96.0f;

// Left panel — marks scoreboard
static constexpr float    LEFT_PANEL_X       = 15.0f;
static constexpr float    LEFT_PANEL_W       = 495.0f;
static constexpr float    MARKS_TOP_Y        = 180.0f;
static constexpr float    MARKS_HEADER_H     = 45.0f;
static constexpr float    MARKS_ROW_H        = 60.0f;
static constexpr float    MARKS_LABEL_W      = 60.0f;

// Board segment colors for Cricket states
static constexpr Color COLOR_CLOSED     = {60, 100, 200};   // Current player closed (blue)
static constexpr Color COLOR_THREATENED = {220, 140, 30};   // Opponent closed, player hasn't (orange)
static constexpr Color COLOR_HIT_FLASH  = {255, 255, 200};  // Dart hit flash (yellow)


CricketGame::CricketGame(CricketScoring scoring, bool randomNumbers, bool teamsMode)
    : Game("Cricket")
    , m_scoring(scoring)
    , m_randomNumbers(randomNumbers)
    , m_teamsMode(teamsMode)
    , m_fontId(INVALID_FONT_ID)
    , m_largeFontId(INVALID_FONT_ID)
    , m_board()
{
}


Status CricketGame::init(FrameID frameId)
{
    m_frameId = frameId;

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

    m_board = DartBoard::create(m_world, GameLayout::BOARD_CENTER_X, GameLayout::BOARD_CENTER_Y,
                                GameLayout::BOARD_SCALE, m_fontId);

    // Set up target numbers
    if(m_randomNumbers)
    {
        std::random_device rd;
        std::mt19937 gen(rd());
        std::vector<uint8_t> pool;
        for(uint8_t i = 1; i <= 20; i++) pool.push_back(i);
        std::shuffle(pool.begin(), pool.end(), gen);
        for(int i = 0; i < 6; i++) m_targets[i] = pool[i];
        // Sort descending for display
        std::sort(m_targets, m_targets + 6, std::greater<uint8_t>());
        m_targets[6] = 0; // Bull always last
    }
    else
    {
        // Standard: 20, 19, 18, 17, 16, 15, Bull
        m_targets[0] = 20;
        m_targets[1] = 19;
        m_targets[2] = 18;
        m_targets[3] = 17;
        m_targets[4] = 16;
        m_targets[5] = 15;
        m_targets[6] = 0; // Bull
    }

    // Initialize teams mode if enabled and teams exist
    if(m_teamsMode && getTeamCount() > 0)
    {
        m_turnTracker.init();
    }
    else
    {
        m_teamsMode = false; // Fall back if no teams configured
    }

    // Initialize per-entity state (teams in teams mode, players otherwise)
    uint8_t entityCount = m_teamsMode ? m_turnTracker.teamCount() : getPlayerCount();
    m_players.resize(entityCount);
    for(auto& p : m_players)
    {
        std::fill(std::begin(p.marks), std::end(p.marks), 0);
        p.score = 0;
    }

    updateBoardColors();

    return STATUS_OK;
}


int CricketGame::findTargetIndex(uint8_t section) const
{
    for(int i = 0; i < CRICKET_NUM_TARGETS; i++)
    {
        if(m_targets[i] == section) return i;
    }
    return -1;
}


bool CricketGame::isNumberClosedByAll(int targetIdx) const
{
    for(size_t i = 0; i < m_players.size(); i++)
    {
        if(m_players[i].marks[targetIdx] < 3) return false;
    }
    return true;
}


bool CricketGame::anyOpponentClosed(int targetIdx) const
{
    for(size_t i = 0; i < m_players.size(); i++)
    {
        if(i != m_currentPlayerIndex && m_players[i].marks[targetIdx] >= 3)
            return true;
    }
    return false;
}


bool CricketGame::checkWinCondition(uint8_t playerIdx) const
{
    auto& player = m_players[playerIdx];

    // Must have closed all targets
    for(int i = 0; i < CRICKET_NUM_TARGETS; i++)
    {
        if(player.marks[i] < 3) return false;
    }

    switch(m_scoring)
    {
        case CricketScoring::NoScore:
            return true;

        case CricketScoring::Standard:
            // Must have highest (or tied) score
            for(size_t i = 0; i < m_players.size(); i++)
            {
                if(i != playerIdx && m_players[i].score > player.score)
                    return false;
            }
            return true;

        case CricketScoring::CutThroat:
            // Must have lowest (or tied) score
            for(size_t i = 0; i < m_players.size(); i++)
            {
                if(i != playerIdx && m_players[i].score < player.score)
                    return false;
            }
            return true;
    }
    return false;
}


uint16_t CricketGame::getTargetFaceValue(int targetIdx) const
{
    uint8_t section = m_targets[targetIdx];
    return (section == 0) ? 25 : static_cast<uint16_t>(section);
}


void CricketGame::updateBoardColors()
{
    // Start from default board palette
    m_board.unhighlightAll();

    auto& player = m_players[m_currentPlayerIndex];

    for(uint8_t i = 0; i < static_cast<uint8_t>(DartSegment::COUNT); i++)
    {
        DartSegment seg = static_cast<DartSegment>(i);
        uint8_t section = getSegmentSection(seg);
        int targetIdx = findTargetIndex(section);

        if(targetIdx >= 0 && !isNumberClosedByAll(targetIdx))
        {
            if(player.marks[targetIdx] >= 3 && m_blinkOn)
            {
                // Current player closed: blink blue (attacking/scoring position)
                m_board.setSegmentColor(seg, COLOR_CLOSED);
            }
            else if(anyOpponentClosed(targetIdx) && m_blinkOn)
            {
                // Opponent closed, player hasn't: blink orange (under threat)
                m_board.setSegmentColor(seg, COLOR_THREATENED);
            }
            // else: needs closing, no one else has — stays at default board palette
        }
        // else: non-target or closed by all — stays at default board palette
    }
}


void CricketGame::update(float deltaTime)
{
    if(m_gameOver) return;

    auto& player = m_players[m_currentPlayerIndex];

    // Blink active targets on/off
    if(updateBlink(deltaTime, m_blinkTimer, m_blinkOn))
    {
        updateBoardColors();
        // Re-flash target hits on blink-on
        if(m_blinkOn)
        {
            for(auto& hit : m_hitSegments)
            {
                int ti = findTargetIndex(getSegmentSection(hit.segment));
                if(ti < 0) continue;
                // Skip if this dart just closed the target (let blue show)
                if(player.marks[ti] >= 3 && !hit.wasAlreadyClosed) continue;
                m_board.setSegmentColor(hit.segment, COLOR_HIT_FLASH);
            }
        }
    }

    // Drain dart events
    uint32_t landed = consumeDartLandedCount();

    DartPosition pos;
    bool hasPosition = popDartPosition(pos);
    if(landed > 0 && !hasPosition && m_throwsRemaining > 0)
    {
        m_statusText = "Hold";
    }

    while(hasPosition)
    {
        if(m_throwsRemaining == 0)
        {
            hasPosition = popDartPosition(pos);
            continue;
        }

        auto segment = polarToSegment(pos.angle, pos.normalizedRadius);
        if(!segment.has_value())
        {
            m_statusText = "Waiting for Throw";
            hasPosition = popDartPosition(pos);
            continue;
        }

        uint8_t section = getSegmentSection(segment.value());
        int targetIdx = findTargetIndex(section);

        m_throwsRemaining--;
        m_statusText = "Waiting for Throw";

        // Track hit — record whether the target was already closed before this dart
        bool alreadyClosed = (targetIdx >= 0 && player.marks[targetIdx] >= 3);
        m_hitSegments.push_back({segment.value(), alreadyClosed});
        m_hitPositions.push_back(pos);
        m_board.setSegmentColor(segment.value(), COLOR_HIT_FLASH);

        if(targetIdx < 0)
        {
            // Not a target number — wasted dart
            if(m_throwsRemaining == 0) m_waitingForCollect = true;
            hasPosition = popDartPosition(pos);
            continue;
        }

        // Determine marks to add
        uint8_t marksToAdd;
        if(section == 0)
        {
            // Bull: outer = 1 mark, inner = 2 marks
            DartRing ring = getSegmentRing(segment.value());
            marksToAdd = (ring == DartRing::InnerBull) ? 2 : 1;
        }
        else
        {
            marksToAdd = getSegmentMultiplier(segment.value());
        }

        // Apply marks
        uint8_t prevMarks = player.marks[targetIdx];
        player.marks[targetIdx] += marksToAdd;

        // Track first player to close this target
        if(prevMarks < 3 && player.marks[targetIdx] >= 3 && m_firstToClose[targetIdx] < 0)
        {
            m_firstToClose[targetIdx] = static_cast<int8_t>(m_currentPlayerIndex);
        }

        // Only score if the number was already closed before this dart
        uint8_t scoringHits = (prevMarks >= 3) ? marksToAdd : 0;

        // Apply scoring if number isn't fully closed by all players
        if(scoringHits > 0 && !isNumberClosedByAll(targetIdx))
        {
            uint16_t faceValue = getTargetFaceValue(targetIdx);

            switch(m_scoring)
            {
                case CricketScoring::Standard:
                    player.score += scoringHits * faceValue;
                    break;

                case CricketScoring::CutThroat:
                    for(size_t i = 0; i < m_players.size(); i++)
                    {
                        if(i != m_currentPlayerIndex && m_players[i].marks[targetIdx] < 3)
                        {
                            m_players[i].score += scoringHits * faceValue;
                        }
                    }
                    break;

                case CricketScoring::NoScore:
                    break;
            }
        }

        // Update board colors to reflect new state
        updateBoardColors();
        // Re-flash target hits (skip darts that just closed a target)
        for(auto& hit : m_hitSegments)
        {
            int ti = findTargetIndex(getSegmentSection(hit.segment));
            if(ti < 0) continue;
            if(player.marks[ti] >= 3 && !hit.wasAlreadyClosed) continue;
            m_board.setSegmentColor(hit.segment, COLOR_HIT_FLASH);
        }

        // Check for win — defer game over until darts are collected
        if(checkWinCondition(m_currentPlayerIndex))
        {
            m_throwsRemaining = 0;
            m_waitingForCollect = true;
            break;
        }

        if(m_throwsRemaining == 0)
        {
            m_waitingForCollect = true;
        }

        hasPosition = popDartPosition(pos);
    }

    // Board clear — check for win or advance turn
    if(m_waitingForCollect && isBoardClear())
    {
        m_hitSegments.clear();
        m_hitPositions.clear();
        m_blinkTimer = 0.0f;
        m_blinkOn = true;
        m_waitingForCollect = false;

        // Check if current player won
        if(checkWinCondition(m_currentPlayerIndex))
        {
            m_gameOver = true;
            m_winnerIndex = m_currentPlayerIndex;
            m_gameOverCursor = 0;
            return;
        }

        if(m_teamsMode)
        {
            m_turnTracker.advance();
            m_currentPlayerIndex = m_turnTracker.currentTeam();
        }
        else
        {
            m_currentPlayerIndex = (m_currentPlayerIndex + 1) % getPlayerCount();
        }
        m_throwsRemaining = 3;
        m_statusText = "Waiting for Throw";

        updateBoardColors();
    }
}


void CricketGame::render()
{
    m_board.enqueueRender(getFrameId());
    renderHitMarkers(getFrameId(), m_hitPositions,
                     GameLayout::BOARD_CENTER_X, GameLayout::BOARD_CENTER_Y,
                     GameLayout::BOARD_SCALE);
    renderMarksScoreboard();
    renderPointScores();

    // Show "GAME!" banner while waiting for dart collection after a win
    if(m_waitingForCollect && checkWinCondition(m_currentPlayerIndex))
    {
        std::string winnerName = m_teamsMode
            ? m_turnTracker.teamName(m_currentPlayerIndex)
            : getPlayerName(getPlayerByIndex(m_currentPlayerIndex));
        renderAnnouncementBanner(getFrameId(), m_largeFontId, winnerName, "GAME!");
    }

    if(m_gameOver)
    {
        renderGameOver();
    }
}


// ============================================================================
// Marks scoreboard (left panel)
// ============================================================================

void CricketGame::renderMarksScoreboard()
{
    FrameID fid = getFrameId();
    uint8_t entityCount = static_cast<uint8_t>(m_players.size());
    float colW = (entityCount > 0)
        ? (LEFT_PANEL_W - MARKS_LABEL_W) / entityCount
        : 0.0f;

    // Header — entity name abbreviations (team names or player names)
    for(uint8_t p = 0; p < entityCount; p++)
    {
        std::string name = m_teamsMode
            ? m_turnTracker.teamName(p)
            : getPlayerName(getPlayerByIndex(p));
        bool isCurrent = (p == m_currentPlayerIndex);

        // Truncate name to fit column width
        TTF_Font* nameFont = getFont(m_fontId);
        float maxNameW = colW - 12.0f;
        if(nameFont)
        {
            int nameW = 0, nameH = 0;
            TTF_GetStringSize(nameFont, name.c_str(), 0, &nameW, &nameH);
            if(static_cast<float>(nameW) > maxNameW && name.length() > 1)
            {
                int dotsW = 0;
                TTF_GetStringSize(nameFont, "...", 0, &dotsW, &nameH);
                float maxBeforeDots = maxNameW - static_cast<float>(dotsW);
                while(name.length() > 1)
                {
                    name.pop_back();
                    TTF_GetStringSize(nameFont, name.c_str(), 0, &nameW, &nameH);
                    if(static_cast<float>(nameW) <= maxBeforeDots) break;
                }
                name += "...";
            }
        }

        // Column highlight for current player
        if(isCurrent)
        {
            float colX = LEFT_PANEL_X + MARKS_LABEL_W + p * colW;
            float colH = MARKS_HEADER_H + CRICKET_NUM_TARGETS * MARKS_ROW_H;

            auto bg = std::make_shared<RenderShape>();
            bg->m_type   = ShapeType::Box;
            bg->m_color  = {50, 50, 80};
            bg->m_x      = colX;
            bg->m_y      = MARKS_TOP_Y;
            bg->m_z      = GameLayout::SIDEBAR_Z;
            bg->m_width  = colW;
            bg->m_height = colH;
            renderQueueAdd(fid, bg);
        }

        auto hdr = std::make_shared<RenderText>();
        hdr->m_text     = name;
        hdr->m_color    = isCurrent ? Color{255, 255, 255} : Color{160, 160, 160};
        hdr->m_fontId   = m_fontId;
        hdr->m_rotation = 0.0f;
        hdr->m_scaleX   = 1.0f;
        hdr->m_scaleY   = 1.0f;
        hdr->m_x        = LEFT_PANEL_X + MARKS_LABEL_W + p * colW + 6.0f;
        hdr->m_y        = MARKS_TOP_Y + 6.0f;
        hdr->m_z        = GameLayout::SIDEBAR_Z + 1;
        renderQueueAdd(fid, hdr);
    }

    // Rows — one per target
    for(int t = 0; t < CRICKET_NUM_TARGETS; t++)
    {
        float rowY = MARKS_TOP_Y + MARKS_HEADER_H + t * MARKS_ROW_H;

        // Row separator line
        auto line = std::make_shared<RenderShape>();
        line->m_type   = ShapeType::Box;
        line->m_color  = {60, 60, 60};
        line->m_x      = LEFT_PANEL_X;
        line->m_y      = rowY;
        line->m_z      = GameLayout::SIDEBAR_Z;
        line->m_width  = LEFT_PANEL_W;
        line->m_height = 1.0f;
        renderQueueAdd(fid, line);

        // Target label
        std::string label = (m_targets[t] == 0) ? "B" : std::to_string(m_targets[t]);

        auto lbl = std::make_shared<RenderText>();
        lbl->m_text     = label;
        lbl->m_color    = {200, 200, 200};
        lbl->m_fontId   = m_fontId;
        lbl->m_rotation = 0.0f;
        lbl->m_scaleX   = 1.0f;
        lbl->m_scaleY   = 1.0f;
        lbl->m_x        = LEFT_PANEL_X + 6.0f;
        lbl->m_y        = rowY + 12.0f;
        lbl->m_z        = GameLayout::SIDEBAR_Z + 1;
        renderQueueAdd(fid, lbl);

        // Marks per entity
        for(uint8_t p = 0; p < entityCount; p++)
        {
            uint8_t marks = m_players[p].marks[t];
            float cellX = LEFT_PANEL_X + MARKS_LABEL_W + p * colW;
            float cellCenterX = cellX + colW * 0.5f;
            float cellCenterY = rowY + MARKS_ROW_H * 0.5f;

            if(marks == 0)
            {
                // No marks — leave blank
            }
            else if(marks >= 3)
            {
                // Closed — blue dot for first to close, green for others
                Color dotColor = (m_firstToClose[t] == static_cast<int8_t>(p))
                    ? Color{60, 120, 220} : Color{60, 200, 80};

                auto circle = std::make_shared<RenderShape>();
                circle->m_type   = ShapeType::Circle;
                circle->m_color  = dotColor;
                circle->m_x      = cellCenterX;
                circle->m_y      = cellCenterY;
                circle->m_z      = GameLayout::SIDEBAR_Z + 2;
                circle->m_width  = 24.0f; // diameter
                circle->m_height = 24.0f;
                renderQueueAdd(fid, circle);
            }
            else
            {
                // 1 mark = "/", 2 marks = "X"
                std::string sym = (marks == 1) ? "/" : "X";

                TTF_Font* font = getFont(m_fontId);
                int symW = 0, symH = 0;
                if(font) TTF_GetStringSize(font, sym.c_str(), 0, &symW, &symH);

                auto symText = std::make_shared<RenderText>();
                symText->m_text     = sym;
                symText->m_color    = {220, 220, 220};
                symText->m_fontId   = m_fontId;
                symText->m_rotation = 0.0f;
                symText->m_scaleX   = 1.0f;
                symText->m_scaleY   = 1.0f;
                symText->m_x        = cellCenterX - symW * 0.5f;
                symText->m_y        = cellCenterY - symH * 0.5f;
                symText->m_z        = GameLayout::SIDEBAR_Z + 2;
                renderQueueAdd(fid, symText);
            }
        }
    }
}


// ============================================================================
// Point scores (right panel)
// ============================================================================

void CricketGame::renderPointScores()
{
    uint8_t entityCount = static_cast<uint8_t>(m_players.size());
    std::vector<ScoreboardEntry> entries;
    entries.reserve(entityCount);

    for(uint8_t i = 0; i < entityCount; i++)
    {
        std::string name = m_teamsMode
            ? m_turnTracker.teamName(i)
            : getPlayerName(getPlayerByIndex(i));

        std::string scoreStr;
        Color scoreColor = {255, 255, 255};

        if(m_scoring == CricketScoring::NoScore)
        {
            uint8_t closed = 0;
            for(int t = 0; t < CRICKET_NUM_TARGETS; t++)
            {
                if(m_players[i].marks[t] >= 3) closed++;
            }
            scoreStr = std::to_string(closed) + "/7";
        }
        else
        {
            scoreStr = std::to_string(m_players[i].score);
            if(m_scoring == CricketScoring::CutThroat && m_players[i].score > 0)
            {
                scoreColor = {255, 140, 140};
            }
        }

        entries.push_back({name, scoreStr, scoreColor});
    }

    renderScoreboardPanel(getFrameId(), m_fontId, entries, m_currentPlayerIndex);
}


// ============================================================================
// Lifecycle
// ============================================================================

void CricketGame::shutdown()
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


GameBarInfo CricketGame::getBarInfo() const
{
    if(m_teamsMode)
    {
        // Show the actual thrower's name, not the team
        uint8_t throwerIdx = m_turnTracker.currentPlayerIndex();
        return makeBarInfo(m_gameOver, m_waitingForCollect,
                           throwerIdx, m_throwsRemaining, m_statusText);
    }
    return makeBarInfo(m_gameOver, m_waitingForCollect,
                       m_currentPlayerIndex, m_throwsRemaining, m_statusText);
}


uint8_t CricketGame::getMaxPlayers() const
{
    return 6;
}


// ============================================================================
// Input handling (game-over menu only)
// ============================================================================

void CricketGame::onKeyDown(uint32_t keycode)
{
    if(!m_gameOver) return;

    auto action = handleGameOverKey(keycode, m_gameOverCursor);
    if(action == GameOverAction::Restart)       restartCurrentGame();
    else if(action == GameOverAction::MainMenu) loadGame(std::make_shared<MainMenu>());
}


void CricketGame::onGamepadButton(uint8_t button, bool pressed)
{
    if(!pressed || !m_gameOver) return;

    auto action = handleGameOverGamepad(button, m_gameOverCursor);
    if(action == GameOverAction::Restart)       restartCurrentGame();
    else if(action == GameOverAction::MainMenu) loadGame(std::make_shared<MainMenu>());
}


void CricketGame::renderGameOver()
{
    std::string winnerName = m_teamsMode
        ? m_turnTracker.teamName(m_winnerIndex)
        : getPlayerName(getPlayerByIndex(m_winnerIndex));
    renderGameOverOverlay(getFrameId(), m_largeFontId, m_fontId,
                          winnerName, m_gameOverCursor);
}
