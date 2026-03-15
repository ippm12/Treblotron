/**
 * x01.cpp
 *
 * X01 game implementation.
 */

#include "x01.hpp"
#include "dart/dart_defs.hpp"
#include "frame/render_queue.hpp"
#include "game_lib/game_manager.hpp"
#include "game_lib/components/render_shape.hpp"
#include "game_lib/components/render_text.hpp"
#include "games/main_menu.hpp"
#include "players/players.hpp"

#include <SDL3/SDL_keycode.h>
#include <SDL3/SDL_gamepad.h>
#include <SDL3_ttf/SDL_ttf.h>


static constexpr float  BOARD_CENTER_X  = 640.0f;
static constexpr float  BOARD_CENTER_Y  = 290.0f;
static constexpr float  BOARD_SCALE     = 1.2f;
static constexpr float  FONT_SIZE       = 20.0f;
static constexpr float  LARGE_FONT_SIZE = 64.0f;
static constexpr float  BLINK_PERIOD    = 1.2f; // seconds per full on/off cycle

// Right panel — all-player scoreboard
static constexpr float    RIGHT_PANEL_X     = 940.0f;
static constexpr float    RIGHT_PANEL_W     = 320.0f;
static constexpr float    SCOREBOARD_TOP_Y  = 120.0f;
static constexpr float    SCOREBOARD_ROW_H  = 50.0f;
static constexpr float    SCOREBOARD_ROW_PAD = 8.0f;
static constexpr float    SCOREBOARD_ACCENT_W = 4.0f;
static constexpr uint32_t SIDEBAR_Z         = 100;
static constexpr size_t   MAX_NAME_DISPLAY  = 10; // Sidebar name truncation limit

// Left panel — current player detail
static constexpr float LEFT_PANEL_X      = 10.0f;
static constexpr float LEFT_NAME_Y       = 120.0f;
static constexpr float LEFT_SCORE_Y      = 170.0f;
static constexpr float LEFT_PROGRESS_Y   = 280.0f;
static constexpr float LEFT_DARTS_Y      = 320.0f;
static constexpr float LEFT_DART_ROW_H   = 28.0f;


X01Game::X01Game(X01Variant variant)
    : Game("X01")
    , m_variant(variant)
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
    m_board = DartBoard::create(m_world, BOARD_CENTER_X, BOARD_CENTER_Y, BOARD_SCALE, m_fontId);

    // Initialize per-player scores
    uint16_t startScore = static_cast<uint16_t>(m_variant);
    m_playerScores.assign(getPlayerCount(), startScore);
    m_turnStartScore = startScore;
    m_turnScoreProgression.clear();

    return STATUS_OK;
}


void X01Game::update(float deltaTime)
{
    // Game over — stop processing darts (pause is handled by GameManager)
    if(m_gameOver)
    {
        return;
    }

    uint16_t& score = m_playerScores[m_currentPlayerIndex];

    // Blink highlighted segments
    if(!m_hitSegments.empty())
    {
        m_blinkTimer += deltaTime;
        if(m_blinkTimer >= BLINK_PERIOD * 0.5f)
        {
            m_blinkTimer = 0.0f;
            m_blinkOn = !m_blinkOn;

            for(auto seg : m_hitSegments)
            {
                if(m_blinkOn)
                {
                    m_board.highlightSegment(seg);
                }
                else
                {
                    m_board.unhighlightSegment(seg);
                }
            }
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

        // Subtract from score
        if(points <= score)
        {
            score -= points;
        }
        // else: bust (to be implemented with proper bust rules)

        m_throwsRemaining--;
        m_statusText = "Waiting for Throw";

        // Check for win
        if(score == 0)
        {
            m_gameOver = true;
            m_winnerIndex = m_currentPlayerIndex;
            m_gameOverCursor = 0;
            LOG_INFO(GAME_MANAGER_LOG_ID, "X01: Player {} wins!", m_currentPlayerIndex);
            break;
        }

        // Track turn progression
        m_turnScoreProgression.push_back(score);

        // Track and highlight the hit segment
        m_hitSegments.push_back(segment.value());
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
        m_blinkTimer = 0.0f;
        m_blinkOn = true;
        m_waitingForCollect = false;

        // Advance to next player
        m_currentPlayerIndex = (m_currentPlayerIndex + 1) % getPlayerCount();
        m_throwsRemaining = 3;
        m_turnScoreProgression.clear();
        m_turnStartScore = m_playerScores[m_currentPlayerIndex];
        m_statusText = "Waiting for Throw";
    }
}


void X01Game::render()
{
    // Enqueue the dart board (GameManager handles clear/flush/present)
    m_board.enqueueRender(getFrameId());
    renderRightScoreboard();
    renderLeftPlayerDetail();

    if(m_gameOver)
    {
        renderGameOver();
    }
}


void X01Game::renderRightScoreboard()
{
    FrameID fid = getFrameId();
    TTF_Font* font = getFont(m_fontId);
    uint8_t playerCount = getPlayerCount();

    // Header
    auto header = std::make_shared<RenderText>();
    header->m_text     = "SCOREBOARD";
    header->m_color    = {200, 200, 200};
    header->m_fontId   = m_fontId;
    header->m_rotation = 0.0f;
    header->m_scaleX   = 1.0f;
    header->m_scaleY   = 1.0f;
    header->m_x        = RIGHT_PANEL_X;
    header->m_y        = SCOREBOARD_TOP_Y - 35.0f;
    header->m_z        = SIDEBAR_Z;
    renderQueueAdd(fid, header);

    for(uint8_t i = 0; i < playerCount; i++)
    {
        float rowY = SCOREBOARD_TOP_Y + i * SCOREBOARD_ROW_H;
        bool isCurrent = (i == m_currentPlayerIndex);

        // Row background
        auto bg = std::make_shared<RenderShape>();
        bg->m_type   = ShapeType::Box;
        bg->m_color  = isCurrent ? Color{80, 80, 120} : Color{50, 50, 50};
        bg->m_x      = RIGHT_PANEL_X;
        bg->m_y      = rowY;
        bg->m_z      = SIDEBAR_Z;
        bg->m_width  = RIGHT_PANEL_W;
        bg->m_height = SCOREBOARD_ROW_H - 4.0f;
        renderQueueAdd(fid, bg);

        // Accent bar for current player
        if(isCurrent)
        {
            auto accent = std::make_shared<RenderShape>();
            accent->m_type   = ShapeType::Box;
            accent->m_color  = {100, 180, 255};
            accent->m_x      = RIGHT_PANEL_X;
            accent->m_y      = rowY;
            accent->m_z      = SIDEBAR_Z + 1;
            accent->m_width  = SCOREBOARD_ACCENT_W;
            accent->m_height = SCOREBOARD_ROW_H - 4.0f;
            renderQueueAdd(fid, accent);
        }

        Color textColor = isCurrent ? Color{255, 255, 255} : Color{180, 180, 180};

        // Player name (truncated to match bottom bar limit)
        PlayerID pid = getPlayerByIndex(i);
        std::string name = getPlayerName(pid);
        if(name.length() > MAX_NAME_DISPLAY)
        {
            name = name.substr(0, MAX_NAME_DISPLAY - 3) + "...";
        }

        auto nameText = std::make_shared<RenderText>();
        nameText->m_text     = name;
        nameText->m_color    = textColor;
        nameText->m_fontId   = m_fontId;
        nameText->m_rotation = 0.0f;
        nameText->m_scaleX   = 1.0f;
        nameText->m_scaleY   = 1.0f;
        nameText->m_x        = RIGHT_PANEL_X + SCOREBOARD_ROW_PAD + SCOREBOARD_ACCENT_W;
        nameText->m_y        = rowY + SCOREBOARD_ROW_PAD;
        nameText->m_z        = SIDEBAR_Z + 1;
        renderQueueAdd(fid, nameText);

        // Player score (right-aligned)
        std::string scoreStr = std::to_string(m_playerScores[i]);
        float scoreX = RIGHT_PANEL_X + RIGHT_PANEL_W - SCOREBOARD_ROW_PAD;
        if(font)
        {
            int textW = 0;
            int textH = 0;
            TTF_GetStringSize(font, scoreStr.c_str(), 0, &textW, &textH);
            scoreX -= static_cast<float>(textW);
        }

        auto scoreText = std::make_shared<RenderText>();
        scoreText->m_text     = scoreStr;
        scoreText->m_color    = textColor;
        scoreText->m_fontId   = m_fontId;
        scoreText->m_rotation = 0.0f;
        scoreText->m_scaleX   = 1.0f;
        scoreText->m_scaleY   = 1.0f;
        scoreText->m_x        = scoreX;
        scoreText->m_y        = rowY + SCOREBOARD_ROW_PAD;
        scoreText->m_z        = SIDEBAR_Z + 1;
        renderQueueAdd(fid, scoreText);
    }
}


void X01Game::renderLeftPlayerDetail()
{
    FrameID fid = getFrameId();
    uint16_t score = m_playerScores[m_currentPlayerIndex];
    PlayerID pid = getPlayerByIndex(m_currentPlayerIndex);
    std::string name = getPlayerName(pid);

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
    nameText->m_z        = SIDEBAR_Z;
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
    scoreText->m_z        = SIDEBAR_Z;
    renderQueueAdd(fid, scoreText);

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
    progText->m_z        = SIDEBAR_Z;
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
        dartText->m_z        = SIDEBAR_Z;
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
        totalText->m_z        = SIDEBAR_Z;
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
    GameBarInfo info;

    if(m_gameOver)
    {
        info.state = GameState::Blank;
        return info;
    }

    if(m_waitingForCollect)
    {
        info.state      = GameState::CollectDarts;
        info.playerName = getPlayerName(getPlayerByIndex(m_currentPlayerIndex));
        info.statusText = "Collect Darts";
    }
    else
    {
        info.state           = GameState::PlayerTurn;
        info.playerName      = getPlayerName(getPlayerByIndex(m_currentPlayerIndex));
        info.throwsRemaining = m_throwsRemaining;
        info.statusText      = m_statusText;
    }

    return info;
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
    if(!m_gameOver)
    {
        return;
    }

    switch(keycode)
    {
        case SDLK_UP:
        case SDLK_DOWN:
            m_gameOverCursor = (m_gameOverCursor == 0) ? 1 : 0;
            break;
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
            if(m_gameOverCursor == 0)
            {
                restartCurrentGame();
            }
            else
            {
                loadGame(std::make_shared<MainMenu>());
            }
            break;
        default:
            break;
    }
}


void X01Game::onGamepadButton(uint8_t button, bool pressed)
{
    if(!pressed) return;

    switch(button)
    {
        case SDL_GAMEPAD_BUTTON_DPAD_UP:
            onKeyDown(SDLK_UP);
            break;
        case SDL_GAMEPAD_BUTTON_DPAD_DOWN:
            onKeyDown(SDLK_DOWN);
            break;
        case SDL_GAMEPAD_BUTTON_SOUTH:
            onKeyDown(SDLK_RETURN);
            break;
        default:
            break;
    }
}


// ============================================================================
// Game-over overlay rendering
// ============================================================================

static constexpr float    OVERLAY_Z       = 500;
static constexpr float    PANEL_W         = 500.0f;
static constexpr float    PANEL_H         = 300.0f;
static constexpr float    PANEL_X         = (1280.0f - PANEL_W) * 0.5f;
static constexpr float    PANEL_Y         = (620.0f - PANEL_H) * 0.5f; // center above status bar
static constexpr float    GO_ROW_H        = 55.0f;
static constexpr float    GO_ROW_W        = 300.0f;

void X01Game::renderGameOver()
{
    FrameID fid = getFrameId();

    // Dark overlay
    auto overlay = std::make_shared<RenderShape>();
    overlay->m_type   = ShapeType::Box;
    overlay->m_color  = {20, 20, 20};
    overlay->m_x      = 0.0f;
    overlay->m_y      = 0.0f;
    overlay->m_z      = OVERLAY_Z;
    overlay->m_width  = 1280.0f;
    overlay->m_height = 620.0f;
    renderQueueAdd(fid, overlay);

    // Center panel
    auto panel = std::make_shared<RenderShape>();
    panel->m_type   = ShapeType::Box;
    panel->m_color  = {50, 50, 55};
    panel->m_x      = PANEL_X;
    panel->m_y      = PANEL_Y;
    panel->m_z      = OVERLAY_Z + 1;
    panel->m_width  = PANEL_W;
    panel->m_height = PANEL_H;
    renderQueueAdd(fid, panel);

    // Winner text
    PlayerID winnerId = getPlayerByIndex(m_winnerIndex);
    std::string winnerName = getPlayerName(winnerId);
    std::string winText = winnerName + " Wins!";

    // Center the winner text horizontally
    TTF_Font* largeFont = getFont(m_largeFontId);
    int winTextW = 0, winTextH = 0;
    if(largeFont)
    {
        TTF_GetStringSize(largeFont, winText.c_str(), 0, &winTextW, &winTextH);
    }

    auto winLabel = std::make_shared<RenderText>();
    winLabel->m_text     = winText;
    winLabel->m_color    = {255, 220, 80};
    winLabel->m_fontId   = m_largeFontId;
    winLabel->m_rotation = 0.0f;
    winLabel->m_scaleX   = 0.7f;
    winLabel->m_scaleY   = 0.7f;
    float scaledW = winTextW * 0.7f;
    winLabel->m_x        = PANEL_X + (PANEL_W - scaledW) * 0.5f;
    winLabel->m_y        = PANEL_Y + 30.0f;
    winLabel->m_z        = OVERLAY_Z + 2;
    renderQueueAdd(fid, winLabel);

    // Menu options
    const char* options[] = {"Restart", "Main Menu"};
    float optionsStartY = PANEL_Y + 130.0f;

    for(int i = 0; i < 2; i++)
    {
        float rowY = optionsStartY + i * GO_ROW_H;
        bool isSelected = (i == m_gameOverCursor);
        float rowX = PANEL_X + (PANEL_W - GO_ROW_W) * 0.5f;

        // Row highlight
        if(isSelected)
        {
            auto bg = std::make_shared<RenderShape>();
            bg->m_type   = ShapeType::Box;
            bg->m_color  = {70, 70, 120};
            bg->m_x      = rowX;
            bg->m_y      = rowY;
            bg->m_z      = OVERLAY_Z + 2;
            bg->m_width  = GO_ROW_W;
            bg->m_height = GO_ROW_H - 6.0f;
            renderQueueAdd(fid, bg);
        }

        // Center option text in the row
        TTF_Font* font = getFont(m_fontId);
        int optW = 0, optH = 0;
        if(font)
        {
            TTF_GetStringSize(font, options[i], 0, &optW, &optH);
        }

        auto optText = std::make_shared<RenderText>();
        optText->m_text     = options[i];
        optText->m_color    = isSelected ? Color{255, 255, 255} : Color{160, 160, 170};
        optText->m_fontId   = m_fontId;
        optText->m_rotation = 0.0f;
        optText->m_scaleX   = 1.0f;
        optText->m_scaleY   = 1.0f;
        optText->m_x        = rowX + (GO_ROW_W - optW) * 0.5f;
        optText->m_y        = rowY + (GO_ROW_H - 6.0f - optH) * 0.5f;
        optText->m_z        = OVERLAY_Z + 3;
        renderQueueAdd(fid, optText);
    }
}
