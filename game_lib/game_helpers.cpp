/**
 * game_helpers.cpp
 *
 * Shared helper functions for dart games.
 */

#include "game_lib/game_helpers.hpp"
#include "game_lib/components/render_shape.hpp"
#include "game_lib/components/render_text.hpp"
#include "dart/dart_board_geometry.hpp"
#include "frame/render_queue.hpp"
#include "players/players.hpp"

#include <SDL3/SDL_keycode.h>
#include <SDL3/SDL_gamepad.h>
#include <SDL3_ttf/SDL_ttf.h>

#include <algorithm>
#include <cmath>
#include <memory>


// ============================================================================
// Game-over overlay
// ============================================================================

static constexpr float GO_PANEL_X = 390.0f;
static constexpr float GO_PANEL_Y = 160.0f;
static constexpr float GO_PANEL_W = 500.0f;
static constexpr float GO_PANEL_H = 280.0f;
static constexpr float GO_ROW_H   = 50.0f;
static constexpr float GO_ROW_W   = 300.0f;


void renderGameOverOverlay(FrameID frameId, FontID largeFontId, FontID fontId,
                           const std::string& winnerName, uint8_t menuCursor)
{
    // Dark overlay
    auto overlay = std::make_shared<RenderShape>();
    overlay->m_type   = ShapeType::Box;
    overlay->m_color  = {20, 20, 20};
    overlay->m_x      = 0.0f;
    overlay->m_y      = 0.0f;
    overlay->m_z      = GameLayout::OVERLAY_Z;
    overlay->m_width  = 1280.0f;
    overlay->m_height = 620.0f;
    renderQueueAdd(frameId, overlay);

    // Center panel
    auto panel = std::make_shared<RenderShape>();
    panel->m_type   = ShapeType::Box;
    panel->m_color  = {50, 50, 55};
    panel->m_x      = GO_PANEL_X;
    panel->m_y      = GO_PANEL_Y;
    panel->m_z      = GameLayout::OVERLAY_Z + 1;
    panel->m_width  = GO_PANEL_W;
    panel->m_height = GO_PANEL_H;
    renderQueueAdd(frameId, panel);

    // Winner text
    std::string winText = winnerName + " Wins!";

    TTF_Font* largeFont = getFont(largeFontId);
    int winTextW = 0, winTextH = 0;
    if(largeFont)
    {
        TTF_GetStringSize(largeFont, winText.c_str(), 0, &winTextW, &winTextH);
    }

    auto winLabel = std::make_shared<RenderText>();
    winLabel->m_text     = winText;
    winLabel->m_color    = {255, 220, 80};
    winLabel->m_fontId   = largeFontId;
    winLabel->m_rotation = 0.0f;
    winLabel->m_scaleX   = 0.7f;
    winLabel->m_scaleY   = 0.7f;
    float scaledW = winTextW * 0.7f;
    winLabel->m_x        = GO_PANEL_X + (GO_PANEL_W - scaledW) * 0.5f;
    winLabel->m_y        = GO_PANEL_Y + 30.0f;
    winLabel->m_z        = GameLayout::OVERLAY_Z + 2;
    renderQueueAdd(frameId, winLabel);

    // Menu options
    const char* options[] = {"Restart", "Main Menu"};
    float optionsStartY = GO_PANEL_Y + 130.0f;

    for(int i = 0; i < 2; i++)
    {
        float rowY = optionsStartY + i * GO_ROW_H;
        bool isSelected = (i == menuCursor);
        float rowX = GO_PANEL_X + (GO_PANEL_W - GO_ROW_W) * 0.5f;

        if(isSelected)
        {
            auto bg = std::make_shared<RenderShape>();
            bg->m_type   = ShapeType::Box;
            bg->m_color  = {70, 70, 120};
            bg->m_x      = rowX;
            bg->m_y      = rowY;
            bg->m_z      = GameLayout::OVERLAY_Z + 2;
            bg->m_width  = GO_ROW_W;
            bg->m_height = GO_ROW_H - 6.0f;
            renderQueueAdd(frameId, bg);
        }

        TTF_Font* font = getFont(fontId);
        int optW = 0, optH = 0;
        if(font)
        {
            TTF_GetStringSize(font, options[i], 0, &optW, &optH);
        }

        auto optText = std::make_shared<RenderText>();
        optText->m_text     = options[i];
        optText->m_color    = isSelected ? Color{255, 255, 255} : Color{160, 160, 170};
        optText->m_fontId   = fontId;
        optText->m_rotation = 0.0f;
        optText->m_scaleX   = 1.0f;
        optText->m_scaleY   = 1.0f;
        optText->m_x        = rowX + (GO_ROW_W - optW) * 0.5f;
        optText->m_y        = rowY + (GO_ROW_H - 6.0f - optH) * 0.5f;
        optText->m_z        = GameLayout::OVERLAY_Z + 3;
        renderQueueAdd(frameId, optText);
    }
}


GameOverAction handleGameOverKey(uint32_t keycode, uint8_t& cursor)
{
    switch(keycode)
    {
        case SDLK_UP:
        case SDLK_DOWN:
            cursor = (cursor == 0) ? 1 : 0;
            return GameOverAction::None;

        case SDLK_RETURN:
        case SDLK_KP_ENTER:
            return (cursor == 0) ? GameOverAction::Restart : GameOverAction::MainMenu;

        default:
            return GameOverAction::None;
    }
}


GameOverAction handleGameOverGamepad(uint8_t button, uint8_t& cursor)
{
    switch(button)
    {
        case SDL_GAMEPAD_BUTTON_DPAD_UP:
        case SDL_GAMEPAD_BUTTON_DPAD_DOWN:
            cursor = (cursor == 0) ? 1 : 0;
            return GameOverAction::None;

        case SDL_GAMEPAD_BUTTON_SOUTH:
            return (cursor == 0) ? GameOverAction::Restart : GameOverAction::MainMenu;

        default:
            return GameOverAction::None;
    }
}


// ============================================================================
// Scoreboard panel
// ============================================================================

static constexpr float  SB_ROW_PAD    = 8.0f;
static constexpr float  SB_ACCENT_W   = 4.0f;
static constexpr size_t SB_MAX_NAME   = 10;


void renderScoreboardPanel(FrameID frameId, FontID fontId,
                           const std::vector<ScoreboardEntry>& entries,
                           uint8_t currentPlayerIndex)
{
    TTF_Font* font = getFont(fontId);

    // Header
    auto header = std::make_shared<RenderText>();
    header->m_text     = "SCOREBOARD";
    header->m_color    = {200, 200, 200};
    header->m_fontId   = fontId;
    header->m_rotation = 0.0f;
    header->m_scaleX   = 1.0f;
    header->m_scaleY   = 1.0f;
    header->m_x        = GameLayout::RIGHT_PANEL_X;
    header->m_y        = GameLayout::SCORE_TOP_Y - 35.0f;
    header->m_z        = GameLayout::SIDEBAR_Z;
    renderQueueAdd(frameId, header);

    for(size_t i = 0; i < entries.size(); i++)
    {
        float rowY = GameLayout::SCORE_TOP_Y + i * GameLayout::SCORE_ROW_H;
        bool isCurrent = (static_cast<uint8_t>(i) == currentPlayerIndex);

        // Row background
        auto bg = std::make_shared<RenderShape>();
        bg->m_type   = ShapeType::Box;
        bg->m_color  = isCurrent ? Color{80, 80, 120} : Color{50, 50, 50};
        bg->m_x      = GameLayout::RIGHT_PANEL_X;
        bg->m_y      = rowY;
        bg->m_z      = GameLayout::SIDEBAR_Z;
        bg->m_width  = GameLayout::RIGHT_PANEL_W;
        bg->m_height = GameLayout::SCORE_ROW_H - 4.0f;
        renderQueueAdd(frameId, bg);

        // Accent bar for current player
        if(isCurrent)
        {
            auto accent = std::make_shared<RenderShape>();
            accent->m_type   = ShapeType::Box;
            accent->m_color  = {100, 180, 255};
            accent->m_x      = GameLayout::RIGHT_PANEL_X;
            accent->m_y      = rowY;
            accent->m_z      = GameLayout::SIDEBAR_Z + 1;
            accent->m_width  = SB_ACCENT_W;
            accent->m_height = GameLayout::SCORE_ROW_H - 4.0f;
            renderQueueAdd(frameId, accent);
        }

        Color textColor = isCurrent ? Color{255, 255, 255} : Color{180, 180, 180};

        // Player name (truncated)
        std::string name = entries[i].name;
        if(name.length() > SB_MAX_NAME)
        {
            name = name.substr(0, SB_MAX_NAME - 3) + "...";
        }

        auto nameText = std::make_shared<RenderText>();
        nameText->m_text     = name;
        nameText->m_color    = textColor;
        nameText->m_fontId   = fontId;
        nameText->m_rotation = 0.0f;
        nameText->m_scaleX   = 1.0f;
        nameText->m_scaleY   = 1.0f;
        nameText->m_x        = GameLayout::RIGHT_PANEL_X + SB_ROW_PAD + SB_ACCENT_W;
        nameText->m_y        = rowY + SB_ROW_PAD;
        nameText->m_z        = GameLayout::SIDEBAR_Z + 1;
        renderQueueAdd(frameId, nameText);

        // Value (right-aligned)
        const std::string& val = entries[i].value;
        float valX = GameLayout::RIGHT_PANEL_X + GameLayout::RIGHT_PANEL_W - SB_ROW_PAD;
        if(font)
        {
            int textW = 0, textH = 0;
            TTF_GetStringSize(font, val.c_str(), 0, &textW, &textH);
            valX -= static_cast<float>(textW);
        }

        auto valText = std::make_shared<RenderText>();
        valText->m_text     = val;
        valText->m_color    = entries[i].valueColor;
        valText->m_fontId   = fontId;
        valText->m_rotation = 0.0f;
        valText->m_scaleX   = 1.0f;
        valText->m_scaleY   = 1.0f;
        valText->m_x        = valX;
        valText->m_y        = rowY + SB_ROW_PAD;
        valText->m_z        = GameLayout::SIDEBAR_Z + 1;
        renderQueueAdd(frameId, valText);
    }
}


// ============================================================================
// Blink helper
// ============================================================================

bool updateBlink(float deltaTime, float& blinkTimer, bool& blinkOn, float blinkPeriod)
{
    blinkTimer += deltaTime;
    if(blinkTimer >= blinkPeriod * 0.5f)
    {
        blinkTimer = 0.0f;
        blinkOn = !blinkOn;
        return true;
    }
    return false;
}


// ============================================================================
// Hit markers
// ============================================================================

static constexpr float  HIT_MARKER_DIAMETER = 8.0f;
static constexpr Color  HIT_MARKER_COLOR    = {160, 60, 220};
static constexpr uint32_t HIT_MARKER_Z      = 10; // Above board segments and labels

void renderHitMarkers(FrameID frameId,
                      const std::vector<DartPosition>& positions,
                      float boardCenterX, float boardCenterY, float boardScale)
{
    float scaledRadius = DartBoardGeometry::BASE_RADIUS * boardScale;

    for(const auto& pos : positions)
    {
        float angleRad = pos.angle * (3.14159265f / 180.0f);
        float px = boardCenterX + std::cos(angleRad) * pos.normalizedRadius * scaledRadius;
        float py = boardCenterY + std::sin(angleRad) * pos.normalizedRadius * scaledRadius;

        auto marker = std::make_shared<RenderShape>();
        marker->m_type   = ShapeType::Circle;
        marker->m_color  = HIT_MARKER_COLOR;
        marker->m_x      = px;
        marker->m_y      = py;
        marker->m_z      = HIT_MARKER_Z;
        marker->m_width  = HIT_MARKER_DIAMETER;
        marker->m_height = HIT_MARKER_DIAMETER;
        renderQueueAdd(frameId, marker);
    }
}


// ============================================================================
// Bar info builder
// ============================================================================

// ============================================================================
// Team-based player ordering
// ============================================================================

std::vector<uint8_t> buildInterleavedPlayerOrder()
{
    uint8_t playerCount = getPlayerCount();
    uint8_t teamCount = getTeamCount();

    // No teams: sequential order
    if(teamCount == 0)
    {
        std::vector<uint8_t> order(playerCount);
        for(uint8_t i = 0; i < playerCount; i++)
        {
            order[i] = i;
        }
        return order;
    }

    // Collect player indices per team
    std::vector<std::vector<uint8_t>> teamPlayers;
    for(uint8_t t = 0; t < teamCount; t++)
    {
        TeamID tid = getTeamByIndex(t);
        teamPlayers.push_back(getPlayerIndicesForTeam(tid));
    }

    // Round-robin interleave across teams
    std::vector<uint8_t> order;
    order.reserve(playerCount);
    bool placed = true;
    for(size_t round = 0; placed; round++)
    {
        placed = false;
        for(size_t t = 0; t < teamPlayers.size(); t++)
        {
            if(round < teamPlayers[t].size())
            {
                order.push_back(teamPlayers[t][round]);
                placed = true;
            }
        }
    }

    return order;
}


uint8_t advanceInterleavedPlayer(uint8_t currentOrderPosition,
                                  const std::vector<uint8_t>& order)
{
    if(order.empty()) return 0;
    return static_cast<uint8_t>((currentOrderPosition + 1) % order.size());
}


// ============================================================================
// Bar info builder
// ============================================================================

// ============================================================================
// Team turn tracker
// ============================================================================

void TeamTurnTracker::init()
{
    m_teams.clear();
    m_currentTeam = 0;

    uint8_t tc = getTeamCount();
    for(uint8_t t = 0; t < tc; t++)
    {
        TeamID tid = getTeamByIndex(t);
        TeamRoster roster;
        roster.teamId = tid;
        roster.playerIndices = getPlayerIndicesForTeam(tid);
        roster.nextThrower = 0;
        m_teams.push_back(std::move(roster));
    }
}


uint8_t TeamTurnTracker::teamCount() const
{
    return static_cast<uint8_t>(m_teams.size());
}


uint8_t TeamTurnTracker::currentTeam() const
{
    return m_currentTeam;
}


uint8_t TeamTurnTracker::currentPlayerIndex() const
{
    if(m_teams.empty()) return 0;
    const auto& roster = m_teams[m_currentTeam];
    return roster.playerIndices[roster.nextThrower];
}


const std::string& TeamTurnTracker::teamName(uint8_t teamIndex) const
{
    static const std::string empty;
    if(teamIndex >= m_teams.size()) return empty;
    return getTeamName(m_teams[teamIndex].teamId);
}


void TeamTurnTracker::advance()
{
    if(m_teams.empty()) return;

    // Move to next team
    m_currentTeam = (m_currentTeam + 1) % static_cast<uint8_t>(m_teams.size());

    // Rotate the thrower within the new team
    auto& roster = m_teams[m_currentTeam];
    if(!roster.playerIndices.empty())
    {
        roster.nextThrower = (roster.nextThrower + 1) % static_cast<uint8_t>(roster.playerIndices.size());
    }
}


// ============================================================================
// Bar info builder
// ============================================================================

GameBarInfo makeBarInfo(bool gameOver, bool waitingForCollect,
                        uint8_t playerIndex, uint8_t throwsRemaining,
                        const std::string& statusText)
{
    GameBarInfo info;

    if(gameOver)
    {
        info.state = GameState::Blank;
        return info;
    }

    if(waitingForCollect)
    {
        info.state = GameState::CollectDarts;
        info.playerName = getPlayerName(getPlayerByIndex(playerIndex));
        info.throwsRemaining = 0;
        info.statusText = "Collect Darts";
        return info;
    }

    info.state = GameState::PlayerTurn;
    info.playerName = getPlayerName(getPlayerByIndex(playerIndex));
    info.throwsRemaining = throwsRemaining;
    info.statusText = statusText;
    return info;
}
