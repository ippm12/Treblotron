/**
 * main_menu.cpp
 *
 * Startup menu with a 2-tall card grid, game settings pages,
 * and player management.
 */

#include "games/main_menu.hpp"
#include "calibration.hpp"
#include "vision_debug.hpp"
#include "game_lib/game_registry.hpp"
#include "game_lib/game_manager.hpp"
#include "game_lib/components/render_shape.hpp"
#include "game_lib/components/render_text.hpp"
#include "frame/render_queue.hpp"
#include "players/players.hpp"

#include <SDL3/SDL_keycode.h>
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>


// ============================================================================
// Layout constants
// ============================================================================

static constexpr float    WINDOW_W      = 1920.0f;
static constexpr float    WINDOW_H      = 1080.0f;

// Card grid
static constexpr float    LEFT_MARGIN   = 60.0f;
static constexpr float    TOP_MARGIN    = 120.0f;    // space for title
static constexpr float    CARD_W        = 540.0f;
static constexpr float    CARD_H        = 330.0f;
static constexpr float    CARD_GAP      = 30.0f;
static constexpr int      VISIBLE_COLS  = 3;
static constexpr uint32_t CARD_Z        = 10;

// Font sizes
static constexpr float    TITLE_PT      = 72.0f;
static constexpr float    CARD_PT       = 42.0f;
static constexpr float    SMALL_PT      = 39.0f;

// Settings sub-screen
static constexpr float    SETTINGS_LEFT = 150.0f;
static constexpr float    SETTINGS_TOP  = 150.0f;
static constexpr float    ROW_HEIGHT    = 83.0f;
static constexpr float    ROW_PAD       = 18.0f;
static constexpr float    OPTION_X      = 900.0f;
static constexpr uint32_t SETTINGS_Z    = 10;

// Player settings
static constexpr size_t   MAX_NAME_LEN  = 20;


// ============================================================================
// Construction
// ============================================================================

MainMenu::MainMenu()
    : Game("MainMenu")
    , m_state(MenuState::CardGrid)
    , m_cursorCol(0)
    , m_cursorRow(0)
    , m_scrollOffset(0)
    , m_totalCols(0)
    , m_selectedGameIndex(0)
    , m_settingsCursor(0)
    , m_playerCursor(0)
    , m_playerSettingsScroll(0)
    , m_renaming(false)
    , m_renamingTeam(false)
    , m_showEmptyTeamWarning(false)
    , m_showDuplicateNameWarning(false)
    , m_showNoTeamsWarning(false)
    , m_showNoPlayersWarning(false)
    , m_showSimWarning(false)
    , m_titleFontId(INVALID_FONT_ID)
    , m_cardFontId(INVALID_FONT_ID)
    , m_smallFontId(INVALID_FONT_ID)
{
}


// ============================================================================
// Lifecycle
// ============================================================================

Status MainMenu::init(FrameID frameId)
{
    m_frameId = frameId;

    m_titleFontId = loadFont("assets/fonts/Roboto-Regular.ttf", TITLE_PT);
    if(m_titleFontId == INVALID_FONT_ID)
    {
        return STATUS_ERROR_GENERIC;
    }

    m_cardFontId = loadFont("assets/fonts/Roboto-Regular.ttf", CARD_PT);
    if(m_cardFontId == INVALID_FONT_ID)
    {
        return STATUS_ERROR_GENERIC;
    }

    m_smallFontId = loadFont("assets/fonts/Roboto-Regular.ttf", SMALL_PT);
    if(m_smallFontId == INVALID_FONT_ID)
    {
        return STATUS_ERROR_GENERIC;
    }

    // Build card list: Player Settings first, then one card per registered game
    m_cards.clear();
    m_cards.push_back({CardType::PlayerSettings, 0});
    m_cards.push_back({CardType::Calibration, 0});
    m_cards.push_back({CardType::VisionDebug, 0});

    const auto& games = getRegisteredGames();
    for(size_t i = 0; i < games.size(); i++)
    {
        m_cards.push_back({CardType::Game, i});
    }

    m_totalCols = columnCount();
    m_cursorCol = 0;
    m_cursorRow = 0;
    m_scrollOffset = 0;
    m_state = MenuState::CardGrid;

    m_inputHints.init();
    m_virtualKeyboard.init(m_smallFontId, m_cardFontId);

    return STATUS_OK;
}


void MainMenu::update(float /*deltaTime*/)
{
    // All interaction is event-driven
}


void MainMenu::render()
{
    switch(m_state)
    {
        case MenuState::CardGrid:       renderCardGrid();       break;
        case MenuState::GameSettings:   renderGameSettings();   break;
        case MenuState::PlayerSettings: renderPlayerSettings(); break;
    }
}


void MainMenu::shutdown()
{
    m_virtualKeyboard.close();
    m_inputHints.shutdown();

    if(m_smallFontId != INVALID_FONT_ID)
    {
        unloadFont(m_smallFontId);
        m_smallFontId = INVALID_FONT_ID;
    }
    if(m_cardFontId != INVALID_FONT_ID)
    {
        unloadFont(m_cardFontId);
        m_cardFontId = INVALID_FONT_ID;
    }
    if(m_titleFontId != INVALID_FONT_ID)
    {
        unloadFont(m_titleFontId);
        m_titleFontId = INVALID_FONT_ID;
    }
}


uint8_t MainMenu::getMaxPlayers() const
{
    return 0;  // No player limit — skip validation
}


bool MainMenu::isPauseable() const
{
    return false;
}


// ============================================================================
// Input dispatch
// ============================================================================

void MainMenu::onKeyDown(uint32_t keycode)
{
    switch(m_state)
    {
        case MenuState::CardGrid:       handleCardGridKey(keycode);       break;
        case MenuState::GameSettings:   handleGameSettingsKey(keycode);   break;
        case MenuState::PlayerSettings: handlePlayerSettingsKey(keycode); break;
    }
}


void MainMenu::onTextInput(const char* text)
{
    if(m_state != MenuState::PlayerSettings || text == nullptr)
    {
        return;
    }

    if(!m_renaming && !m_renamingTeam)
    {
        return;
    }

    // Route to virtual keyboard if it's open
    if(m_virtualKeyboard.isOpen())
    {
        m_virtualKeyboard.handleTextInput(text);
        return;
    }

    // Append text to the active rename buffer (may be multi-byte UTF-8)
    std::string input(text);
    if(m_renaming)
    {
        if(m_renameBuffer.length() + input.length() <= MAX_NAME_LEN)
        {
            m_renameBuffer += input;
        }
    }
    else if(m_renamingTeam)
    {
        if(m_teamRenameBuffer.length() + input.length() <= MAX_NAME_LEN)
        {
            m_teamRenameBuffer += input;
        }
    }
}


void MainMenu::onGamepadButton(uint8_t button, bool pressed)
{
    if(!pressed) return;

    // Map gamepad buttons to equivalent keyboard actions
    switch(button)
    {
        case SDL_GAMEPAD_BUTTON_DPAD_UP:
            onKeyDown(SDLK_UP);
            break;
        case SDL_GAMEPAD_BUTTON_DPAD_DOWN:
            onKeyDown(SDLK_DOWN);
            break;
        case SDL_GAMEPAD_BUTTON_DPAD_LEFT:
            onKeyDown(SDLK_LEFT);
            break;
        case SDL_GAMEPAD_BUTTON_DPAD_RIGHT:
            onKeyDown(SDLK_RIGHT);
            break;
        case SDL_GAMEPAD_BUTTON_SOUTH:  // A button
            onKeyDown(SDLK_RETURN);
            break;
        case SDL_GAMEPAD_BUTTON_EAST:   // B button
            onKeyDown(SDLK_ESCAPE);
            break;
        case SDL_GAMEPAD_BUTTON_WEST:   // X button
            onKeyDown(SDLK_DELETE);
            break;
        case SDL_GAMEPAD_BUTTON_NORTH:  // Y button
            onKeyDown(SDLK_TAB);
            break;
        case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER:  // LB button
            onKeyDown(SDLK_HOME);
            break;
        default:
            break;
    }
}


// ============================================================================
// Card Grid
// ============================================================================

int MainMenu::columnCount() const
{
    return static_cast<int>((m_cards.size() + 1) / 2);
}


int MainMenu::cardIndexAt(int col, int row) const
{
    int idx = col * 2 + row;
    if(idx < 0 || idx >= static_cast<int>(m_cards.size()))
    {
        return -1;
    }
    return idx;
}


void MainMenu::clampCursor()
{
    // Clamp column
    int maxCol = m_totalCols - 1;
    if(maxCol < 0) maxCol = 0;
    if(m_cursorCol > maxCol) m_cursorCol = maxCol;
    if(m_cursorCol < 0) m_cursorCol = 0;

    // Clamp row: if no card at row 1 in this column, move to row 0
    if(cardIndexAt(m_cursorCol, m_cursorRow) < 0)
    {
        m_cursorRow = 0;
    }

    // Adjust scroll offset so cursor is visible
    if(m_cursorCol < m_scrollOffset)
    {
        m_scrollOffset = m_cursorCol;
    }
    if(m_cursorCol >= m_scrollOffset + VISIBLE_COLS)
    {
        m_scrollOffset = m_cursorCol - VISIBLE_COLS + 1;
    }
    int maxScroll = m_totalCols - VISIBLE_COLS;
    if(maxScroll < 0) maxScroll = 0;
    if(m_scrollOffset > maxScroll) m_scrollOffset = maxScroll;
    if(m_scrollOffset < 0) m_scrollOffset = 0;
}


void MainMenu::openCard()
{
    int idx = cardIndexAt(m_cursorCol, m_cursorRow);
    if(idx < 0) return;

    const MenuCard& card = m_cards[idx];
    if(card.type == CardType::PlayerSettings)
    {
        m_state = MenuState::PlayerSettings;
        m_playerCursor = 0;
        m_playerSettingsScroll = 0;
        m_renaming = false;
        m_renamingTeam = false;
        m_showEmptyTeamWarning = false;
        m_showDuplicateNameWarning = false;
    }
    else if(card.type == CardType::Calibration)
    {
#ifdef DARTLENS_USE_SIM
        m_showSimWarning = true;
#else
        loadGame(std::make_shared<CalibrationScreen>());
#endif
    }
    else if(card.type == CardType::VisionDebug)
    {
        loadGame(std::make_shared<VisionDebugScreen>());
    }
    else
    {
        m_state = MenuState::GameSettings;
        m_selectedGameIndex = card.gameIndex;

        // Initialize setting choices to defaults
        const auto& desc = getRegisteredGames()[m_selectedGameIndex];
        m_settingChoices.clear();
        for(const auto& setting : desc.settings)
        {
            m_settingChoices.push_back(setting.defaultIndex);
        }
        m_settingsCursor = 0;
    }
}


void MainMenu::handleCardGridKey(uint32_t keycode)
{
    if(m_showSimWarning)
    {
        m_showSimWarning = false;
        return;
    }

    switch(keycode)
    {
        case SDLK_UP:
            if(m_cursorRow > 0)
            {
                m_cursorRow--;
            }
            clampCursor();
            break;
        case SDLK_DOWN:
            if(m_cursorRow < 1)
            {
                m_cursorRow = 1;
            }
            clampCursor();
            break;
        case SDLK_LEFT:
            m_cursorCol--;
            clampCursor();
            break;
        case SDLK_RIGHT:
            m_cursorCol++;
            clampCursor();
            break;
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
            openCard();
            break;
        default:
            break;
    }
}


void MainMenu::renderCardGrid()
{
    FrameID fid = getFrameId();

    // Title
    {
        std::string title = "DartLens";
        float titleX = LEFT_MARGIN;

        auto text = std::make_shared<RenderText>();
        text->m_text     = title;
        text->m_color    = {255, 255, 255};
        text->m_fontId   = m_titleFontId;
        text->m_rotation = 0.0f;
        text->m_scaleX   = 1.0f;
        text->m_scaleY   = 1.0f;
        text->m_x        = titleX;
        text->m_y        = 22.5f;
        text->m_z        = CARD_Z;
        renderQueueAdd(fid, text);
    }

    // Cards
    int endCol = m_scrollOffset + VISIBLE_COLS;
    if(endCol > m_totalCols) endCol = m_totalCols;

    for(int col = m_scrollOffset; col < endCol; col++)
    {
        for(int row = 0; row < 2; row++)
        {
            int idx = cardIndexAt(col, row);
            if(idx < 0) continue;

            const MenuCard& card = m_cards[idx];
            bool isSelected = (col == m_cursorCol && row == m_cursorRow);

            float cardX = LEFT_MARGIN + (col - m_scrollOffset) * (CARD_W + CARD_GAP);
            float cardY = TOP_MARGIN + row * (CARD_H + CARD_GAP);

            // Card background
            Color bgColor = isSelected ? Color{70, 70, 120} : Color{55, 55, 65};
            auto bg = std::make_shared<RenderShape>();
            bg->m_type   = ShapeType::Box;
            bg->m_color  = bgColor;
            bg->m_x      = cardX;
            bg->m_y      = cardY;
            bg->m_z      = CARD_Z;
            bg->m_width  = CARD_W;
            bg->m_height = CARD_H;
            renderQueueAdd(fid, bg);

            // Selection accent (left border)
            if(isSelected)
            {
                auto accent = std::make_shared<RenderShape>();
                accent->m_type   = ShapeType::Box;
                accent->m_color  = {100, 180, 255};
                accent->m_x      = cardX;
                accent->m_y      = cardY;
                accent->m_z      = CARD_Z + 1;
                accent->m_width  = 8.0f;
                accent->m_height = CARD_H;
                renderQueueAdd(fid, accent);
            }

            // Card title
            std::string cardTitle;
            std::string cardDesc;
            if(card.type == CardType::PlayerSettings)
            {
                cardTitle = "Player Settings";
                uint8_t count = getPlayerCount();
                cardDesc = std::to_string(count) + " player" + (count != 1 ? "s" : "");
            }
            else if(card.type == CardType::Calibration)
            {
                cardTitle = "Calibration";
                cardDesc  = "Camera setup & data collection";
            }
            else if(card.type == CardType::VisionDebug)
            {
                cardTitle = "Vision Debug";
                cardDesc  = "AI model output & dart tracking";
            }
            else
            {
                const auto& desc = getRegisteredGames()[card.gameIndex];
                cardTitle = desc.name;
                cardDesc  = desc.description;
            }

            auto titleText = std::make_shared<RenderText>();
            titleText->m_text     = cardTitle;
            titleText->m_color    = {255, 255, 255};
            titleText->m_fontId   = m_cardFontId;
            titleText->m_rotation = 0.0f;
            titleText->m_scaleX   = 1.0f;
            titleText->m_scaleY   = 1.0f;
            titleText->m_x        = cardX + 30.0f;
            titleText->m_y        = cardY + 45.0f;
            titleText->m_z        = CARD_Z + 2;
            renderQueueAdd(fid, titleText);

            // Card description — scale down if it overflows the card
            float descScaleX = 1.0f;
            float maxDescW = CARD_W - 60.0f;
            TTF_Font* descFont = getFont(m_smallFontId);
            if(descFont)
            {
                int descW = 0, descH = 0;
                TTF_GetStringSize(descFont, cardDesc.c_str(), 0, &descW, &descH);
                if(static_cast<float>(descW) > maxDescW)
                {
                    descScaleX = maxDescW / static_cast<float>(descW);
                }
            }

            auto descText = std::make_shared<RenderText>();
            descText->m_text     = cardDesc;
            descText->m_color    = {160, 160, 170};
            descText->m_fontId   = m_smallFontId;
            descText->m_rotation = 0.0f;
            descText->m_scaleX   = descScaleX;
            descText->m_scaleY   = 1.0f;
            descText->m_x        = cardX + 30.0f;
            descText->m_y        = cardY + 105.0f;
            descText->m_z        = CARD_Z + 2;
            renderQueueAdd(fid, descText);

            // Game cards: show max players and settings summary
            if(card.type == CardType::Game)
            {
                const auto& desc = getRegisteredGames()[card.gameIndex];
                std::string info = "Max " + std::to_string(desc.maxPlayers) + " players";
                if(!desc.settings.empty())
                {
                    info += "  |  " + std::to_string(desc.settings.size()) + " setting"
                          + (desc.settings.size() != 1 ? "s" : "");
                }

                auto infoText = std::make_shared<RenderText>();
                infoText->m_text     = info;
                infoText->m_color    = {120, 120, 130};
                infoText->m_fontId   = m_smallFontId;
                infoText->m_rotation = 0.0f;
                infoText->m_scaleX   = 1.0f;
                infoText->m_scaleY   = 1.0f;
                infoText->m_x        = cardX + 30.0f;
                infoText->m_y        = cardY + CARD_H - 60.0f;
                infoText->m_z        = CARD_Z + 2;
                renderQueueAdd(fid, infoText);
            }
        }
    }

    // Scroll indicators
    if(m_scrollOffset > 0)
    {
        auto arrow = std::make_shared<RenderText>();
        arrow->m_text     = "<";
        arrow->m_color    = {180, 180, 200};
        arrow->m_fontId   = m_titleFontId;
        arrow->m_rotation = 0.0f;
        arrow->m_scaleX   = 1.0f;
        arrow->m_scaleY   = 1.0f;
        arrow->m_x        = 12.0f;
        arrow->m_y        = TOP_MARGIN + CARD_H - 15.0f;
        arrow->m_z        = CARD_Z + 3;
        renderQueueAdd(fid, arrow);
    }
    if(endCol < m_totalCols)
    {
        auto arrow = std::make_shared<RenderText>();
        arrow->m_text     = ">";
        arrow->m_color    = {180, 180, 200};
        arrow->m_fontId   = m_titleFontId;
        arrow->m_rotation = 0.0f;
        arrow->m_scaleX   = 1.0f;
        arrow->m_scaleY   = 1.0f;
        arrow->m_x        = WINDOW_W - 52.5f;
        arrow->m_y        = TOP_MARGIN + CARD_H - 15.0f;
        arrow->m_z        = CARD_Z + 3;
        renderQueueAdd(fid, arrow);
    }

    // Navigation hints (above the game bar at y=620)
    m_inputHints.render(fid, m_smallFontId, LEFT_MARGIN, 862.5f, CARD_Z, {
        {SDLK_UP,     GAMEPAD_ICON_LEFT_STICK,       "navigate"},
        {SDLK_RETURN, SDL_GAMEPAD_BUTTON_SOUTH,      "select"}
    });

    // ---- Sim mode warning popup ----
    if(m_showSimWarning)
    {
        auto overlay = std::make_shared<RenderShape>();
        overlay->m_type   = ShapeType::Box;
        overlay->m_color  = {20, 20, 20};
        overlay->m_x      = 0.0f;
        overlay->m_y      = 0.0f;
        overlay->m_z      = CARD_Z + 80;
        overlay->m_width  = WINDOW_W;
        overlay->m_height = WINDOW_H;
        renderQueueAdd(fid, overlay);

        float panelW = 930.0f;
        float panelH = 180.0f;
        float panelX = (WINDOW_W - panelW) * 0.5f;
        float panelY = (WINDOW_H - panelH) * 0.5f - 45.0f;

        auto panel = std::make_shared<RenderShape>();
        panel->m_type   = ShapeType::Box;
        panel->m_color  = {50, 50, 55};
        panel->m_x      = panelX;
        panel->m_y      = panelY;
        panel->m_z      = CARD_Z + 81;
        panel->m_width  = panelW;
        panel->m_height = panelH;
        renderQueueAdd(fid, panel);

        std::string msg = "Calibration requires live camera mode.";
        TTF_Font* cardFont = getFont(m_cardFontId);

        auto msgText = std::make_shared<RenderText>();
        msgText->m_text     = msg;
        msgText->m_color    = {255, 200, 80};
        msgText->m_fontId   = m_cardFontId;
        msgText->m_rotation = 0.0f;
        msgText->m_scaleX   = 1.0f;
        msgText->m_scaleY   = 1.0f;
        msgText->m_z        = CARD_Z + 82;
        msgText->m_y        = panelY + 37.5f;
        int msgW = 0, msgH = 0;
        if(cardFont) TTF_GetStringSize(cardFont, msg.c_str(), 0, &msgW, &msgH);
        msgText->m_x = panelX + (panelW - msgW) * 0.5f;
        renderQueueAdd(fid, msgText);

        std::string dismiss = "Press any key to dismiss";
        auto dismissText = std::make_shared<RenderText>();
        dismissText->m_text     = dismiss;
        dismissText->m_color    = {140, 140, 150};
        dismissText->m_fontId   = m_smallFontId;
        dismissText->m_rotation = 0.0f;
        dismissText->m_scaleX   = 1.0f;
        dismissText->m_scaleY   = 1.0f;
        dismissText->m_z        = CARD_Z + 82;
        dismissText->m_y        = panelY + 112.5f;
        TTF_Font* smallFont = getFont(m_smallFontId);
        int dW = 0, dH = 0;
        if(smallFont) TTF_GetStringSize(smallFont, dismiss.c_str(), 0, &dW, &dH);
        dismissText->m_x = panelX + (panelW - dW) * 0.5f;
        renderQueueAdd(fid, dismissText);
    }
}


// ============================================================================
// Game Settings sub-screen
// ============================================================================

void MainMenu::handleGameSettingsKey(uint32_t keycode)
{
    // Dismiss warning on any key
    if(m_showNoTeamsWarning)
    {
        m_showNoTeamsWarning = false;
        return;
    }
    if(m_showNoPlayersWarning)
    {
        m_showNoPlayersWarning = false;
        return;
    }

    const auto& desc = getRegisteredGames()[m_selectedGameIndex];
    int settingCount = static_cast<int>(desc.settings.size());
    int totalRows = settingCount + 1;  // settings + Start button

    switch(keycode)
    {
        case SDLK_UP:
            m_settingsCursor--;
            if(m_settingsCursor < 0) m_settingsCursor = totalRows - 1;
            break;
        case SDLK_DOWN:
            m_settingsCursor++;
            if(m_settingsCursor >= totalRows) m_settingsCursor = 0;
            break;
        case SDLK_LEFT:
            if(m_settingsCursor < settingCount)
            {
                // Cycle option left
                size_t optCount = desc.settings[m_settingsCursor].options.size();
                if(optCount > 0)
                {
                    m_settingChoices[m_settingsCursor] =
                        (m_settingChoices[m_settingsCursor] + optCount - 1) % optCount;
                }
            }
            break;
        case SDLK_RIGHT:
            if(m_settingsCursor < settingCount)
            {
                // Cycle option right
                size_t optCount = desc.settings[m_settingsCursor].options.size();
                if(optCount > 0)
                {
                    m_settingChoices[m_settingsCursor] =
                        (m_settingChoices[m_settingsCursor] + 1) % optCount;
                }
            }
            break;
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
            if(m_settingsCursor == settingCount)
            {
                // Check if there are any players
                if(getPlayerCount() == 0)
                {
                    m_showNoPlayersWarning = true;
                    return;
                }

                // Check if any "Teams" setting is enabled but no teams exist
                for(int s = 0; s < settingCount; s++)
                {
                    if(desc.settings[s].name == "Teams" && m_settingChoices[s] > 0
                       && getTeamCount() == 0)
                    {
                        m_showNoTeamsWarning = true;
                        return;
                    }
                }

                // Start game with restart factory
                auto choices = m_settingChoices;
                auto factory = [desc, choices]() -> GamePtr {
                    return desc.createGame(choices);
                };
                auto game = factory();
                if(game)
                {
                    loadGame(game, factory);
                }
            }
            break;
        case SDLK_ESCAPE:
            m_state = MenuState::CardGrid;
            break;
        default:
            break;
    }
}


void MainMenu::renderGameSettings()
{
    FrameID fid = getFrameId();
    const auto& desc = getRegisteredGames()[m_selectedGameIndex];
    int settingCount = static_cast<int>(desc.settings.size());

    // Title
    {
        auto text = std::make_shared<RenderText>();
        text->m_text     = desc.name;
        text->m_color    = {255, 255, 255};
        text->m_fontId   = m_titleFontId;
        text->m_rotation = 0.0f;
        text->m_scaleX   = 1.0f;
        text->m_scaleY   = 1.0f;
        text->m_x        = SETTINGS_LEFT;
        text->m_y        = 45.0f;
        text->m_z        = SETTINGS_Z;
        renderQueueAdd(fid, text);
    }

    // Description
    {
        auto text = std::make_shared<RenderText>();
        text->m_text     = desc.description;
        text->m_color    = {160, 160, 170};
        text->m_fontId   = m_smallFontId;
        text->m_rotation = 0.0f;
        text->m_scaleX   = 1.0f;
        text->m_scaleY   = 1.0f;
        text->m_x        = SETTINGS_LEFT;
        text->m_y        = 127.5f;
        text->m_z        = SETTINGS_Z;
        renderQueueAdd(fid, text);
    }

    // Settings rows
    for(int i = 0; i < settingCount; i++)
    {
        float rowY = SETTINGS_TOP + 45.0f + i * ROW_HEIGHT;
        bool isSelected = (i == m_settingsCursor);

        // Row highlight
        if(isSelected)
        {
            auto bg = std::make_shared<RenderShape>();
            bg->m_type   = ShapeType::Box;
            bg->m_color  = {60, 60, 100};
            bg->m_x      = SETTINGS_LEFT - 15.0f;
            bg->m_y      = rowY - 7.5f;
            bg->m_z      = SETTINGS_Z;
            bg->m_width  = WINDOW_W - 2 * SETTINGS_LEFT + 30.0f;
            bg->m_height = ROW_HEIGHT - 6.0f;
            renderQueueAdd(fid, bg);
        }

        // Setting name
        auto nameText = std::make_shared<RenderText>();
        nameText->m_text     = desc.settings[i].name;
        nameText->m_color    = isSelected ? Color{255, 255, 255} : Color{180, 180, 190};
        nameText->m_fontId   = m_cardFontId;
        nameText->m_rotation = 0.0f;
        nameText->m_scaleX   = 1.0f;
        nameText->m_scaleY   = 1.0f;
        nameText->m_x        = SETTINGS_LEFT;
        nameText->m_y        = rowY + ROW_PAD;
        nameText->m_z        = SETTINGS_Z + 1;
        renderQueueAdd(fid, nameText);

        // Current option value with arrows
        size_t choiceIdx = m_settingChoices[i];
        std::string optionLabel = "<  " + desc.settings[i].options[choiceIdx].label + "  >";

        auto optText = std::make_shared<RenderText>();
        optText->m_text     = optionLabel;
        optText->m_color    = isSelected ? Color{100, 200, 255} : Color{140, 140, 150};
        optText->m_fontId   = m_cardFontId;
        optText->m_rotation = 0.0f;
        optText->m_scaleX   = 1.0f;
        optText->m_scaleY   = 1.0f;
        optText->m_x        = OPTION_X;
        optText->m_y        = rowY + ROW_PAD;
        optText->m_z        = SETTINGS_Z + 1;
        renderQueueAdd(fid, optText);
    }

    // Start Game button
    {
        float btnY = SETTINGS_TOP + 45.0f + settingCount * ROW_HEIGHT;
        bool isSelected = (m_settingsCursor == settingCount);

        if(isSelected)
        {
            auto bg = std::make_shared<RenderShape>();
            bg->m_type   = ShapeType::Box;
            bg->m_color  = {40, 100, 60};
            bg->m_x      = SETTINGS_LEFT - 15.0f;
            bg->m_y      = btnY - 7.5f;
            bg->m_z      = SETTINGS_Z;
            bg->m_width  = 375.0f;
            bg->m_height = ROW_HEIGHT - 6.0f;
            renderQueueAdd(fid, bg);
        }

        auto btnText = std::make_shared<RenderText>();
        btnText->m_text     = "Start Game";
        btnText->m_color    = isSelected ? Color{100, 255, 130} : Color{120, 180, 130};
        btnText->m_fontId   = m_cardFontId;
        btnText->m_rotation = 0.0f;
        btnText->m_scaleX   = 1.0f;
        btnText->m_scaleY   = 1.0f;
        btnText->m_x        = SETTINGS_LEFT;
        btnText->m_y        = btnY + ROW_PAD;
        btnText->m_z        = SETTINGS_Z + 1;
        renderQueueAdd(fid, btnText);
    }

    // Hints
    m_inputHints.render(fid, m_smallFontId, SETTINGS_LEFT, 862.5f, SETTINGS_Z, {
        {SDLK_UP,     GAMEPAD_ICON_LEFT_STICK,       "navigate"},
        {SDLK_LEFT,   GAMEPAD_ICON_LEFT_STICK,       "change"},
        {SDLK_RETURN, SDL_GAMEPAD_BUTTON_SOUTH,      "confirm"},
        {SDLK_ESCAPE, SDL_GAMEPAD_BUTTON_EAST,       "back"}
    });

    // ---- No-teams warning popup ----
    if(m_showNoTeamsWarning)
    {
        // Dark overlay
        auto overlay = std::make_shared<RenderShape>();
        overlay->m_type   = ShapeType::Box;
        overlay->m_color  = {20, 20, 20};
        overlay->m_x      = 0.0f;
        overlay->m_y      = 0.0f;
        overlay->m_z      = SETTINGS_Z + 80;
        overlay->m_width  = WINDOW_W;
        overlay->m_height = WINDOW_H;
        renderQueueAdd(fid, overlay);

        // Panel
        float panelW = 870.0f;
        float panelH = 210.0f;
        float panelX = (WINDOW_W - panelW) * 0.5f;
        float panelY = (WINDOW_H - panelH) * 0.5f - 45.0f;

        auto panel = std::make_shared<RenderShape>();
        panel->m_type   = ShapeType::Box;
        panel->m_color  = {50, 50, 55};
        panel->m_x      = panelX;
        panel->m_y      = panelY;
        panel->m_z      = SETTINGS_Z + 81;
        panel->m_width  = panelW;
        panel->m_height = panelH;
        renderQueueAdd(fid, panel);

        // Warning text
        std::string msg1 = "Teams mode requires at least";
        std::string msg2 = "one team. Create teams in Player Settings.";

        TTF_Font* cardFont = getFont(m_cardFontId);

        auto line1 = std::make_shared<RenderText>();
        line1->m_text     = msg1;
        line1->m_color    = {255, 200, 80};
        line1->m_fontId   = m_cardFontId;
        line1->m_rotation = 0.0f;
        line1->m_scaleX   = 1.0f;
        line1->m_scaleY   = 1.0f;
        line1->m_z        = SETTINGS_Z + 82;
        line1->m_y        = panelY + 37.5f;
        int w1 = 0, h1 = 0;
        if(cardFont) TTF_GetStringSize(cardFont, msg1.c_str(), 0, &w1, &h1);
        line1->m_x = panelX + (panelW - w1) * 0.5f;
        renderQueueAdd(fid, line1);

        auto line2 = std::make_shared<RenderText>();
        line2->m_text     = msg2;
        line2->m_color    = {255, 200, 80};
        line2->m_fontId   = m_cardFontId;
        line2->m_rotation = 0.0f;
        line2->m_scaleX   = 1.0f;
        line2->m_scaleY   = 1.0f;
        line2->m_z        = SETTINGS_Z + 82;
        line2->m_y        = panelY + 90.0f;
        int w2 = 0, h2 = 0;
        if(cardFont) TTF_GetStringSize(cardFont, msg2.c_str(), 0, &w2, &h2);
        line2->m_x = panelX + (panelW - w2) * 0.5f;
        renderQueueAdd(fid, line2);

        // Dismiss hint
        std::string dismiss = "Press any key to dismiss";
        auto dismissText = std::make_shared<RenderText>();
        dismissText->m_text     = dismiss;
        dismissText->m_color    = {140, 140, 150};
        dismissText->m_fontId   = m_smallFontId;
        dismissText->m_rotation = 0.0f;
        dismissText->m_scaleX   = 1.0f;
        dismissText->m_scaleY   = 1.0f;
        dismissText->m_z        = SETTINGS_Z + 82;
        dismissText->m_y        = panelY + 150.0f;
        TTF_Font* smallFont = getFont(m_smallFontId);
        int w3 = 0, h3 = 0;
        if(smallFont) TTF_GetStringSize(smallFont, dismiss.c_str(), 0, &w3, &h3);
        dismissText->m_x = panelX + (panelW - w3) * 0.5f;
        renderQueueAdd(fid, dismissText);
    }

    // ---- No-players warning popup ----
    if(m_showNoPlayersWarning)
    {
        auto npOverlay = std::make_shared<RenderShape>();
        npOverlay->m_type   = ShapeType::Box;
        npOverlay->m_color  = {20, 20, 20};
        npOverlay->m_x      = 0.0f;
        npOverlay->m_y      = 0.0f;
        npOverlay->m_z      = SETTINGS_Z + 80;
        npOverlay->m_width  = WINDOW_W;
        npOverlay->m_height = WINDOW_H;
        renderQueueAdd(fid, npOverlay);

        float npPanelW = 870.0f;
        float npPanelH = 180.0f;
        float npPanelX = (WINDOW_W - npPanelW) * 0.5f;
        float npPanelY = (WINDOW_H - npPanelH) * 0.5f - 45.0f;

        auto npPanel = std::make_shared<RenderShape>();
        npPanel->m_type   = ShapeType::Box;
        npPanel->m_color  = {50, 50, 55};
        npPanel->m_x      = npPanelX;
        npPanel->m_y      = npPanelY;
        npPanel->m_z      = SETTINGS_Z + 81;
        npPanel->m_width  = npPanelW;
        npPanel->m_height = npPanelH;
        renderQueueAdd(fid, npPanel);

        std::string npMsg = "Add at least one player in Player Settings.";
        TTF_Font* cardFont = getFont(m_cardFontId);

        auto npText = std::make_shared<RenderText>();
        npText->m_text     = npMsg;
        npText->m_color    = {255, 200, 80};
        npText->m_fontId   = m_cardFontId;
        npText->m_rotation = 0.0f;
        npText->m_scaleX   = 1.0f;
        npText->m_scaleY   = 1.0f;
        npText->m_z        = SETTINGS_Z + 82;
        npText->m_y        = npPanelY + 37.5f;
        int npW = 0, npH = 0;
        if(cardFont) TTF_GetStringSize(cardFont, npMsg.c_str(), 0, &npW, &npH);
        npText->m_x = npPanelX + (npPanelW - npW) * 0.5f;
        renderQueueAdd(fid, npText);

        std::string npDismiss = "Press any key to dismiss";
        auto npDismissText = std::make_shared<RenderText>();
        npDismissText->m_text     = npDismiss;
        npDismissText->m_color    = {140, 140, 150};
        npDismissText->m_fontId   = m_smallFontId;
        npDismissText->m_rotation = 0.0f;
        npDismissText->m_scaleX   = 1.0f;
        npDismissText->m_scaleY   = 1.0f;
        npDismissText->m_z        = SETTINGS_Z + 82;
        npDismissText->m_y        = npPanelY + 112.5f;
        TTF_Font* npSmallFont = getFont(m_smallFontId);
        int npDW = 0, npDH = 0;
        if(npSmallFont) TTF_GetStringSize(npSmallFont, npDismiss.c_str(), 0, &npDW, &npDH);
        npDismissText->m_x = npPanelX + (npPanelW - npDW) * 0.5f;
        renderQueueAdd(fid, npDismissText);
    }
}


// ============================================================================
// Player Settings sub-screen
// ============================================================================

/** Helper: compute the total number of selectable rows in the player settings screen. */
static int playerSettingsRowCount()
{
    // players + "Add Player" + teams + "Add Team"
    return getPlayerCount() + 1 + getTeamCount() + 1;
}

/** Helper: clamp the player settings scroll so cursor is visible. */
static constexpr int VISIBLE_SETTINGS_ROWS = 8;

static void clampPlayerSettingsScroll(int cursor, int& scroll)
{
    // Account for the visual divider taking a row of space.
    // The divider appears after the "Add Player" row (logical row = playerCount).
    // Visual row = logical row + (1 if past Add Player, to account for divider).
    int playerCount = getPlayerCount();
    int visualRow = cursor;
    if(cursor > playerCount)
    {
        visualRow = cursor + 1; // +1 for the divider
    }

    if(visualRow < scroll)
    {
        scroll = visualRow;
    }
    if(visualRow >= scroll + VISIBLE_SETTINGS_ROWS)
    {
        scroll = visualRow - VISIBLE_SETTINGS_ROWS + 1;
    }
    if(scroll < 0) scroll = 0;
}


/** Check if a player name is already used by another player (excluding excludeId). */
static bool isPlayerNameTaken(const std::string& name, PlayerID excludeId)
{
    uint8_t count = getPlayerCount();
    for(uint8_t i = 0; i < count; i++)
    {
        PlayerID pid = getPlayerByIndex(i);
        if(pid != excludeId && getPlayerName(pid) == name)
        {
            return true;
        }
    }
    return false;
}

/** Check if a team name is already used by another team (excluding excludeId). */
static bool isTeamNameTaken(const std::string& name, TeamID excludeId)
{
    uint8_t count = getTeamCount();
    for(uint8_t i = 0; i < count; i++)
    {
        TeamID tid = getTeamByIndex(i);
        if(tid != excludeId && getTeamName(tid) == name)
        {
            return true;
        }
    }
    return false;
}

/** Generate a unique player name like "Player 3" that doesn't collide. */
static std::string generateUniquePlayerName()
{
    for(int n = getPlayerCount() + 1; ; n++)
    {
        std::string candidate = "Player " + std::to_string(n);
        if(!isPlayerNameTaken(candidate, INVALID_PLAYER_ID))
        {
            return candidate;
        }
    }
}

/** Generate a unique team name like "Team 2" that doesn't collide. */
static std::string generateUniqueTeamName()
{
    for(int n = getTeamCount() + 1; ; n++)
    {
        std::string candidate = "Team " + std::to_string(n);
        if(!isTeamNameTaken(candidate, INVALID_TEAM_ID))
        {
            return candidate;
        }
    }
}


void MainMenu::handlePlayerSettingsKey(uint32_t keycode)
{
    uint8_t playerCount = getPlayerCount();
    uint8_t teamCount = getTeamCount();
    int totalRows = playerSettingsRowCount();

    // Row ranges:
    // 0..playerCount-1              = player rows
    // playerCount                   = "Add Player"
    // playerCount+1..playerCount+teamCount = team rows
    // playerCount+teamCount+1       = "Add Team"

    // Dismiss warnings on any key
    if(m_showEmptyTeamWarning || m_showDuplicateNameWarning)
    {
        m_showEmptyTeamWarning = false;
        m_showDuplicateNameWarning = false;
        return;
    }

    bool onPlayerRow    = (m_playerCursor < playerCount);
    bool onAddPlayer    = (m_playerCursor == playerCount);
    bool onTeamRow      = (m_playerCursor > playerCount && m_playerCursor <= playerCount + teamCount);
    bool onAddTeam      = (m_playerCursor == playerCount + teamCount + 1);
    (void)onAddPlayer; // used implicitly below
    (void)onAddTeam;

    // If virtual keyboard is open, route input there
    if(m_virtualKeyboard.isOpen())
    {
        VirtualKeyboardResult result = m_virtualKeyboard.handleKey(keycode);
        if(result == VirtualKeyboardResult::Confirmed)
        {
            std::string newName = m_virtualKeyboard.getText();
            if(m_renaming)
            {
                PlayerID pid = getPlayerByIndex(static_cast<uint8_t>(m_playerCursor));
                if(pid != INVALID_PLAYER_ID)
                {
                    if(!newName.empty() && isPlayerNameTaken(newName, pid))
                    {
                        m_showDuplicateNameWarning = true;
                    }
                    else if(!newName.empty())
                    {
                        setPlayerName(pid, newName);
                    }
                }
                m_renaming = false;
            }
            else if(m_renamingTeam)
            {
                int teamIdx = m_playerCursor - playerCount - 1;
                TeamID tid = getTeamByIndex(static_cast<uint8_t>(teamIdx));
                if(tid != INVALID_TEAM_ID)
                {
                    if(!newName.empty() && isTeamNameTaken(newName, tid))
                    {
                        m_showDuplicateNameWarning = true;
                    }
                    else if(!newName.empty())
                    {
                        setTeamName(tid, newName);
                    }
                }
                m_renamingTeam = false;
            }
            m_virtualKeyboard.close();
        }
        else if(result == VirtualKeyboardResult::Cancelled)
        {
            m_renaming = false;
            m_renamingTeam = false;
            m_virtualKeyboard.close();
        }
        return;
    }

    // Physical keyboard rename mode (player)
    if(m_renaming)
    {
        switch(keycode)
        {
            case SDLK_RETURN:
            case SDLK_KP_ENTER:
            {
                if(!m_renameBuffer.empty())
                {
                    PlayerID pid = getPlayerByIndex(static_cast<uint8_t>(m_playerCursor));
                    if(pid != INVALID_PLAYER_ID)
                    {
                        if(isPlayerNameTaken(m_renameBuffer, pid))
                        {
                            m_showDuplicateNameWarning = true;
                        }
                        else
                        {
                            setPlayerName(pid, m_renameBuffer);
                        }
                    }
                }
                m_renaming = false;
                SDL_StopTextInput(SDL_GetKeyboardFocus());
                break;
            }
            case SDLK_ESCAPE:
                m_renaming = false;
                SDL_StopTextInput(SDL_GetKeyboardFocus());
                break;
            case SDLK_BACKSPACE:
                if(!m_renameBuffer.empty())
                {
                    m_renameBuffer.pop_back();
                }
                break;
            default:
                break;
        }
        return;
    }

    // Physical keyboard rename mode (team)
    if(m_renamingTeam)
    {
        switch(keycode)
        {
            case SDLK_RETURN:
            case SDLK_KP_ENTER:
            {
                if(!m_teamRenameBuffer.empty())
                {
                    int teamIdx = m_playerCursor - playerCount - 1;
                    TeamID tid = getTeamByIndex(static_cast<uint8_t>(teamIdx));
                    if(tid != INVALID_TEAM_ID)
                    {
                        if(isTeamNameTaken(m_teamRenameBuffer, tid))
                        {
                            m_showDuplicateNameWarning = true;
                        }
                        else
                        {
                            setTeamName(tid, m_teamRenameBuffer);
                        }
                    }
                }
                m_renamingTeam = false;
                SDL_StopTextInput(SDL_GetKeyboardFocus());
                break;
            }
            case SDLK_ESCAPE:
                m_renamingTeam = false;
                SDL_StopTextInput(SDL_GetKeyboardFocus());
                break;
            case SDLK_BACKSPACE:
                if(!m_teamRenameBuffer.empty())
                {
                    m_teamRenameBuffer.pop_back();
                }
                break;
            default:
                break;
        }
        return;
    }

    // Normal navigation
    switch(keycode)
    {
        case SDLK_UP:
            m_playerCursor--;
            if(m_playerCursor < 0) m_playerCursor = totalRows - 1;
            clampPlayerSettingsScroll(m_playerCursor, m_playerSettingsScroll);
            break;
        case SDLK_DOWN:
            m_playerCursor++;
            if(m_playerCursor >= totalRows) m_playerCursor = 0;
            clampPlayerSettingsScroll(m_playerCursor, m_playerSettingsScroll);
            break;
        case SDLK_LEFT:
            // Cycle team assignment backward on player rows
            if(onPlayerRow && teamCount > 0)
            {
                PlayerID pid = getPlayerByIndex(static_cast<uint8_t>(m_playerCursor));
                if(pid != INVALID_PLAYER_ID)
                {
                    TeamID currentTeam = getPlayerTeam(pid);
                    // Find index of current team, then cycle backward
                    for(uint8_t t = 0; t < teamCount; t++)
                    {
                        if(getTeamByIndex(t) == currentTeam)
                        {
                            uint8_t prevIdx = (t == 0) ? teamCount - 1 : t - 1;
                            setPlayerTeam(pid, getTeamByIndex(prevIdx));
                            break;
                        }
                    }
                }
            }
            break;
        case SDLK_RIGHT:
            // Cycle team assignment forward on player rows
            if(onPlayerRow && teamCount > 0)
            {
                PlayerID pid = getPlayerByIndex(static_cast<uint8_t>(m_playerCursor));
                if(pid != INVALID_PLAYER_ID)
                {
                    TeamID currentTeam = getPlayerTeam(pid);
                    for(uint8_t t = 0; t < teamCount; t++)
                    {
                        if(getTeamByIndex(t) == currentTeam)
                        {
                            uint8_t nextIdx = (t + 1) % teamCount;
                            setPlayerTeam(pid, getTeamByIndex(nextIdx));
                            break;
                        }
                    }
                }
            }
            break;
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
            if(onPlayerRow)
            {
                // Start renaming this player
                PlayerID pid = getPlayerByIndex(static_cast<uint8_t>(m_playerCursor));
                if(pid != INVALID_PLAYER_ID)
                {
                    m_renaming = true;
                    m_renameBuffer = getPlayerName(pid);
                    if(getLastInputDevice() == InputDevice::Gamepad)
                    {
                        m_virtualKeyboard.open(m_renameBuffer, MAX_NAME_LEN);
                    }
                    else
                    {
                        SDL_StartTextInput(SDL_GetKeyboardFocus());
                    }
                }
            }
            else if(onAddPlayer)
            {
                createPlayer(generateUniquePlayerName());
            }
            else if(onTeamRow)
            {
                // Start renaming this team
                int teamIdx = m_playerCursor - playerCount - 1;
                TeamID tid = getTeamByIndex(static_cast<uint8_t>(teamIdx));
                if(tid != INVALID_TEAM_ID)
                {
                    m_renamingTeam = true;
                    m_teamRenameBuffer = getTeamName(tid);
                    if(getLastInputDevice() == InputDevice::Gamepad)
                    {
                        m_virtualKeyboard.open(m_teamRenameBuffer, MAX_NAME_LEN);
                    }
                    else
                    {
                        SDL_StartTextInput(SDL_GetKeyboardFocus());
                    }
                }
            }
            else if(onAddTeam)
            {
                createTeam(generateUniqueTeamName());
            }
            break;
        case SDLK_DELETE:
        case SDLK_BACKSPACE:
            if(onPlayerRow)
            {
                PlayerID pid = getPlayerByIndex(static_cast<uint8_t>(m_playerCursor));
                if(pid != INVALID_PLAYER_ID)
                {
                    removePlayer(pid);
                    int newTotal = playerSettingsRowCount();
                    if(m_playerCursor >= newTotal)
                    {
                        m_playerCursor = newTotal - 1;
                    }
                    clampPlayerSettingsScroll(m_playerCursor, m_playerSettingsScroll);
                }
            }
            else if(onTeamRow)
            {
                int teamIdx = m_playerCursor - playerCount - 1;
                TeamID tid = getTeamByIndex(static_cast<uint8_t>(teamIdx));
                if(tid != INVALID_TEAM_ID)
                {
                    removeTeam(tid);
                    int newTotal = playerSettingsRowCount();
                    if(m_playerCursor >= newTotal)
                    {
                        m_playerCursor = newTotal - 1;
                    }
                    clampPlayerSettingsScroll(m_playerCursor, m_playerSettingsScroll);
                }
            }
            break;
        case SDLK_ESCAPE:
        {
            // Validate: if teams exist, every team must have at least 1 player
            if(teamCount > 0)
            {
                for(uint8_t t = 0; t < teamCount; t++)
                {
                    TeamID tid = getTeamByIndex(t);
                    if(getPlayerIndicesForTeam(tid).empty())
                    {
                        m_showEmptyTeamWarning = true;
                        return;
                    }
                }
            }
            m_state = MenuState::CardGrid;
            break;
        }
        default:
            break;
    }
}


void MainMenu::renderPlayerSettings()
{
    FrameID fid = getFrameId();
    uint8_t playerCount = getPlayerCount();
    uint8_t teamCount = getTeamCount();
    TTF_Font* cardFont = getFont(m_cardFontId);

    // Title
    {
        auto text = std::make_shared<RenderText>();
        text->m_text     = "Player Settings";
        text->m_color    = {255, 255, 255};
        text->m_fontId   = m_titleFontId;
        text->m_rotation = 0.0f;
        text->m_scaleX   = 1.0f;
        text->m_scaleY   = 1.0f;
        text->m_x        = SETTINGS_LEFT;
        text->m_y        = 45.0f;
        text->m_z        = SETTINGS_Z;
        renderQueueAdd(fid, text);
    }

    // Visual rows are laid out as:
    //   0..playerCount-1              = player rows
    //   playerCount                   = "Add Player"
    //   playerCount+1                 = divider / "Teams" header (visual only)
    //   playerCount+2..+1+teamCount   = team rows
    //   playerCount+2+teamCount       = "Add Team"
    // But logical (selectable) cursor rows skip the divider:
    //   0..playerCount-1              = player rows
    //   playerCount                   = "Add Player"
    //   playerCount+1..+teamCount     = team rows
    //   playerCount+teamCount+1       = "Add Team"

    // Helper: convert logical cursor row to visual row (accounting for divider)
    auto logicalToVisualRow = [&](int logical) -> int {
        if(logical > playerCount)
        {
            return logical + 1; // +1 for divider
        }
        return logical;
    };

    float contentStartY = SETTINGS_TOP + 15.0f;

    // ---- Player rows ----
    for(uint8_t i = 0; i < playerCount; i++)
    {
        int visualRow = logicalToVisualRow(i);
        int screenRow = visualRow - m_playerSettingsScroll;
        if(screenRow < 0 || screenRow >= VISIBLE_SETTINGS_ROWS) continue;

        float rowY = contentStartY + screenRow * ROW_HEIGHT;
        bool isSelected = (static_cast<int>(i) == m_playerCursor);

        // Row highlight
        if(isSelected)
        {
            auto bg = std::make_shared<RenderShape>();
            bg->m_type   = ShapeType::Box;
            bg->m_color  = {60, 60, 100};
            bg->m_x      = SETTINGS_LEFT - 15.0f;
            bg->m_y      = rowY - 7.5f;
            bg->m_z      = SETTINGS_Z;
            bg->m_width  = WINDOW_W - 2 * SETTINGS_LEFT + 30.0f;
            bg->m_height = ROW_HEIGHT - 6.0f;
            renderQueueAdd(fid, bg);
        }

        PlayerID pid = getPlayerByIndex(i);
        std::string name;
        if(m_renaming && isSelected)
        {
            if(m_virtualKeyboard.isOpen())
            {
                name = m_virtualKeyboard.getText() + "_";
            }
            else
            {
                name = m_renameBuffer + "_";
            }
        }
        else
        {
            name = (pid != INVALID_PLAYER_ID) ? getPlayerName(pid) : "???";
        }

        float boxY = rowY - 7.5f;
        float boxH = ROW_HEIGHT - 6.0f;
        int textW = 0, textH = 0;
        if(cardFont)
        {
            TTF_GetStringSize(cardFont, name.c_str(), 0, &textW, &textH);
        }

        auto nameText = std::make_shared<RenderText>();
        nameText->m_text     = name;
        nameText->m_color    = isSelected ? Color{255, 255, 255} : Color{180, 180, 190};
        nameText->m_fontId   = m_cardFontId;
        nameText->m_rotation = 0.0f;
        nameText->m_scaleX   = 1.0f;
        nameText->m_scaleY   = 1.0f;
        nameText->m_x        = SETTINGS_LEFT;
        nameText->m_y        = boxY + (boxH - textH) * 0.5f;
        nameText->m_z        = SETTINGS_Z + 1;
        renderQueueAdd(fid, nameText);

        // Team assignment label (right side)
        if(teamCount > 0 && pid != INVALID_PLAYER_ID && !m_renaming)
        {
            TeamID tid = getPlayerTeam(pid);
            std::string teamLabel;
            if(isSelected)
            {
                teamLabel = "<  " + getTeamName(tid) + "  >";
            }
            else
            {
                teamLabel = getTeamName(tid);
            }

            auto teamText = std::make_shared<RenderText>();
            teamText->m_text     = teamLabel;
            teamText->m_color    = isSelected ? Color{100, 200, 255} : Color{120, 120, 140};
            teamText->m_fontId   = m_cardFontId;
            teamText->m_rotation = 0.0f;
            teamText->m_scaleX   = 1.0f;
            teamText->m_scaleY   = 1.0f;
            teamText->m_x        = OPTION_X;
            teamText->m_y        = boxY + (boxH - textH) * 0.5f;
            teamText->m_z        = SETTINGS_Z + 1;
            renderQueueAdd(fid, teamText);
        }

        // Action hints for selected player row (not renaming)
        if(isSelected && !m_renaming)
        {
            float hintsX = (teamCount > 0) ? OPTION_X + 420.0f : OPTION_X;
            float hintsY = boxY + (boxH - 54.0f) * 0.5f;
            m_inputHints.render(fid, m_smallFontId, hintsX, hintsY, SETTINGS_Z, {
                {SDLK_RETURN, SDL_GAMEPAD_BUTTON_SOUTH, "rename"},
                {SDLK_DELETE, SDL_GAMEPAD_BUTTON_WEST,  "remove"}
            });
        }
    }

    // ---- "Add Player" row ----
    {
        int visualRow = logicalToVisualRow(playerCount);
        int screenRow = visualRow - m_playerSettingsScroll;
        if(screenRow >= 0 && screenRow < VISIBLE_SETTINGS_ROWS)
        {
            float rowY = contentStartY + screenRow * ROW_HEIGHT;
            bool isSelected = (m_playerCursor == playerCount);
            float addBoxY = rowY - 7.5f;
            float addBoxH = ROW_HEIGHT - 6.0f;

            if(isSelected)
            {
                auto bg = std::make_shared<RenderShape>();
                bg->m_type   = ShapeType::Box;
                bg->m_color  = {40, 80, 60};
                bg->m_x      = SETTINGS_LEFT - 15.0f;
                bg->m_y      = addBoxY;
                bg->m_z      = SETTINGS_Z;
                bg->m_width  = 450.0f;
                bg->m_height = addBoxH;
                renderQueueAdd(fid, bg);
            }

            std::string addLabel = "+ Add Player";
            int addTextW = 0, addTextH = 0;
            if(cardFont)
            {
                TTF_GetStringSize(cardFont, addLabel.c_str(), 0, &addTextW, &addTextH);
            }

            auto addText = std::make_shared<RenderText>();
            addText->m_text     = addLabel;
            addText->m_color    = isSelected ? Color{100, 255, 130} : Color{100, 170, 120};
            addText->m_fontId   = m_cardFontId;
            addText->m_rotation = 0.0f;
            addText->m_scaleX   = 1.0f;
            addText->m_scaleY   = 1.0f;
            addText->m_x        = SETTINGS_LEFT;
            addText->m_y        = addBoxY + (addBoxH - addTextH) * 0.5f;
            addText->m_z        = SETTINGS_Z + 1;
            renderQueueAdd(fid, addText);
        }
    }

    // ---- Divider / "Teams" header (visual row = playerCount + 1) ----
    {
        int dividerVisualRow = playerCount + 1;
        int screenRow = dividerVisualRow - m_playerSettingsScroll;
        if(screenRow >= 0 && screenRow < VISIBLE_SETTINGS_ROWS)
        {
            float divY = contentStartY + screenRow * ROW_HEIGHT;

            // Divider line
            auto line = std::make_shared<RenderShape>();
            line->m_type   = ShapeType::Box;
            line->m_color  = {80, 80, 90};
            line->m_x      = SETTINGS_LEFT - 15.0f;
            line->m_y      = divY + 7.5f;
            line->m_z      = SETTINGS_Z;
            line->m_width  = WINDOW_W - 2 * SETTINGS_LEFT + 30.0f;
            line->m_height = 3.0f;
            renderQueueAdd(fid, line);

            // "Teams" header text
            auto header = std::make_shared<RenderText>();
            header->m_text     = "Teams";
            header->m_color    = {160, 160, 170};
            header->m_fontId   = m_smallFontId;
            header->m_rotation = 0.0f;
            header->m_scaleX   = 1.0f;
            header->m_scaleY   = 1.0f;
            header->m_x        = SETTINGS_LEFT;
            header->m_y        = divY + 27.0f;
            header->m_z        = SETTINGS_Z + 1;
            renderQueueAdd(fid, header);
        }
    }

    // ---- Team rows ----
    for(uint8_t t = 0; t < teamCount; t++)
    {
        int logicalRow = playerCount + 1 + t;
        int visualRow = logicalToVisualRow(logicalRow);
        int screenRow = visualRow - m_playerSettingsScroll;
        if(screenRow < 0 || screenRow >= VISIBLE_SETTINGS_ROWS) continue;

        float rowY = contentStartY + screenRow * ROW_HEIGHT;
        bool isSelected = (logicalRow == m_playerCursor);

        if(isSelected)
        {
            auto bg = std::make_shared<RenderShape>();
            bg->m_type   = ShapeType::Box;
            bg->m_color  = {60, 60, 100};
            bg->m_x      = SETTINGS_LEFT - 15.0f;
            bg->m_y      = rowY - 7.5f;
            bg->m_z      = SETTINGS_Z;
            bg->m_width  = WINDOW_W - 2 * SETTINGS_LEFT + 30.0f;
            bg->m_height = ROW_HEIGHT - 6.0f;
            renderQueueAdd(fid, bg);
        }

        TeamID tid = getTeamByIndex(t);
        std::string teamName;
        if(m_renamingTeam && isSelected)
        {
            if(m_virtualKeyboard.isOpen())
            {
                teamName = m_virtualKeyboard.getText() + "_";
            }
            else
            {
                teamName = m_teamRenameBuffer + "_";
            }
        }
        else
        {
            teamName = (tid != INVALID_TEAM_ID) ? getTeamName(tid) : "???";
        }

        float boxY = rowY - 7.5f;
        float boxH = ROW_HEIGHT - 6.0f;
        int textW = 0, textH = 0;
        if(cardFont)
        {
            TTF_GetStringSize(cardFont, teamName.c_str(), 0, &textW, &textH);
        }

        auto nameText = std::make_shared<RenderText>();
        nameText->m_text     = teamName;
        nameText->m_color    = isSelected ? Color{255, 255, 255} : Color{180, 180, 190};
        nameText->m_fontId   = m_cardFontId;
        nameText->m_rotation = 0.0f;
        nameText->m_scaleX   = 1.0f;
        nameText->m_scaleY   = 1.0f;
        nameText->m_x        = SETTINGS_LEFT;
        nameText->m_y        = boxY + (boxH - textH) * 0.5f;
        nameText->m_z        = SETTINGS_Z + 1;
        renderQueueAdd(fid, nameText);

        // Action hints for selected team row (not renaming)
        if(isSelected && !m_renamingTeam)
        {
            float hintsY = boxY + (boxH - 54.0f) * 0.5f;
            m_inputHints.render(fid, m_smallFontId, OPTION_X, hintsY, SETTINGS_Z, {
                {SDLK_RETURN, SDL_GAMEPAD_BUTTON_SOUTH, "rename"},
                {SDLK_DELETE, SDL_GAMEPAD_BUTTON_WEST,  "remove"}
            });
        }
    }

    // ---- "Add Team" row ----
    {
        int logicalRow = playerCount + teamCount + 1;
        int visualRow = logicalToVisualRow(logicalRow);
        int screenRow = visualRow - m_playerSettingsScroll;
        if(screenRow >= 0 && screenRow < VISIBLE_SETTINGS_ROWS)
        {
            float rowY = contentStartY + screenRow * ROW_HEIGHT;
            bool isSelected = (m_playerCursor == logicalRow);
            float addBoxY = rowY - 7.5f;
            float addBoxH = ROW_HEIGHT - 6.0f;

            if(isSelected)
            {
                auto bg = std::make_shared<RenderShape>();
                bg->m_type   = ShapeType::Box;
                bg->m_color  = {40, 80, 60};
                bg->m_x      = SETTINGS_LEFT - 15.0f;
                bg->m_y      = addBoxY;
                bg->m_z      = SETTINGS_Z;
                bg->m_width  = 450.0f;
                bg->m_height = addBoxH;
                renderQueueAdd(fid, bg);
            }

            std::string addLabel = "+ Add Team";
            int addTextW = 0, addTextH = 0;
            if(cardFont)
            {
                TTF_GetStringSize(cardFont, addLabel.c_str(), 0, &addTextW, &addTextH);
            }

            auto addText = std::make_shared<RenderText>();
            addText->m_text     = addLabel;
            addText->m_color    = isSelected ? Color{100, 255, 130} : Color{100, 170, 120};
            addText->m_fontId   = m_cardFontId;
            addText->m_rotation = 0.0f;
            addText->m_scaleX   = 1.0f;
            addText->m_scaleY   = 1.0f;
            addText->m_x        = SETTINGS_LEFT;
            addText->m_y        = addBoxY + (addBoxH - addTextH) * 0.5f;
            addText->m_z        = SETTINGS_Z + 1;
            renderQueueAdd(fid, addText);
        }
    }

    // ---- Bottom hints and overlays ----
    if(m_virtualKeyboard.isOpen())
    {
        m_virtualKeyboard.render(fid, SETTINGS_Z + 90);
    }
    else if(m_renaming || m_renamingTeam)
    {
        m_inputHints.render(fid, m_smallFontId, SETTINGS_LEFT, 862.5f, SETTINGS_Z, {
            {SDLK_RETURN, SDL_GAMEPAD_BUTTON_SOUTH, "confirm"},
            {SDLK_ESCAPE, SDL_GAMEPAD_BUTTON_EAST,  "cancel"}
        });
    }
    else
    {
        bool onPlayerRow = (m_playerCursor < playerCount);
        std::vector<InputHint> hints = {
            {SDLK_UP,     GAMEPAD_ICON_LEFT_STICK,    "navigate"},
            {SDLK_RETURN, SDL_GAMEPAD_BUTTON_SOUTH,   "select"},
            {SDLK_ESCAPE, SDL_GAMEPAD_BUTTON_EAST,    "back"}
        };
        if(onPlayerRow && teamCount > 0)
        {
            hints.insert(hints.begin() + 1, {SDLK_LEFT, GAMEPAD_ICON_LEFT_STICK, "team"});
        }
        m_inputHints.render(fid, m_smallFontId, SETTINGS_LEFT, 862.5f, SETTINGS_Z, hints);
    }

    // ---- Warning popups ----
    bool showWarning = m_showEmptyTeamWarning || m_showDuplicateNameWarning;
    std::string warningLine1;
    std::string warningLine2;
    if(m_showEmptyTeamWarning)
    {
        warningLine1 = "Every team must have at least";
        warningLine2 = "one player assigned to it.";
    }
    else if(m_showDuplicateNameWarning)
    {
        warningLine1 = "That name is already taken.";
        warningLine2 = "Names must be unique.";
    }

    if(showWarning)
    {
        // Dark overlay
        auto overlay = std::make_shared<RenderShape>();
        overlay->m_type   = ShapeType::Box;
        overlay->m_color  = {20, 20, 20};
        overlay->m_x      = 0.0f;
        overlay->m_y      = 0.0f;
        overlay->m_z      = SETTINGS_Z + 80;
        overlay->m_width  = WINDOW_W;
        overlay->m_height = WINDOW_H;
        renderQueueAdd(fid, overlay);

        // Panel
        float panelW = 750.0f;
        float panelH = 210.0f;
        float panelX = (WINDOW_W - panelW) * 0.5f;
        float panelY = (WINDOW_H - panelH) * 0.5f - 45.0f;

        auto panel = std::make_shared<RenderShape>();
        panel->m_type   = ShapeType::Box;
        panel->m_color  = {50, 50, 55};
        panel->m_x      = panelX;
        panel->m_y      = panelY;
        panel->m_z      = SETTINGS_Z + 81;
        panel->m_width  = panelW;
        panel->m_height = panelH;
        renderQueueAdd(fid, panel);

        // Warning text
        std::string msg1 = warningLine1;
        std::string msg2 = warningLine2;

        auto line1 = std::make_shared<RenderText>();
        line1->m_text     = msg1;
        line1->m_color    = {255, 200, 80};
        line1->m_fontId   = m_cardFontId;
        line1->m_rotation = 0.0f;
        line1->m_scaleX   = 1.0f;
        line1->m_scaleY   = 1.0f;
        line1->m_z        = SETTINGS_Z + 82;
        line1->m_y        = panelY + 37.5f;
        // Center horizontally
        int w1 = 0, h1 = 0;
        if(cardFont) TTF_GetStringSize(cardFont, msg1.c_str(), 0, &w1, &h1);
        line1->m_x = panelX + (panelW - w1) * 0.5f;
        renderQueueAdd(fid, line1);

        auto line2 = std::make_shared<RenderText>();
        line2->m_text     = msg2;
        line2->m_color    = {255, 200, 80};
        line2->m_fontId   = m_cardFontId;
        line2->m_rotation = 0.0f;
        line2->m_scaleX   = 1.0f;
        line2->m_scaleY   = 1.0f;
        line2->m_z        = SETTINGS_Z + 82;
        line2->m_y        = panelY + 90.0f;
        int w2 = 0, h2 = 0;
        if(cardFont) TTF_GetStringSize(cardFont, msg2.c_str(), 0, &w2, &h2);
        line2->m_x = panelX + (panelW - w2) * 0.5f;
        renderQueueAdd(fid, line2);

        // Dismiss hint
        std::string dismiss = "Press any key to dismiss";
        auto dismissText = std::make_shared<RenderText>();
        dismissText->m_text     = dismiss;
        dismissText->m_color    = {140, 140, 150};
        dismissText->m_fontId   = m_smallFontId;
        dismissText->m_rotation = 0.0f;
        dismissText->m_scaleX   = 1.0f;
        dismissText->m_scaleY   = 1.0f;
        dismissText->m_z        = SETTINGS_Z + 82;
        dismissText->m_y        = panelY + 150.0f;
        TTF_Font* smallFont = getFont(m_smallFontId);
        int w3 = 0, h3 = 0;
        if(smallFont) TTF_GetStringSize(smallFont, dismiss.c_str(), 0, &w3, &h3);
        dismissText->m_x = panelX + (panelW - w3) * 0.5f;
        renderQueueAdd(fid, dismissText);
    }
}
