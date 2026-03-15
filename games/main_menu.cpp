/**
 * main_menu.cpp
 *
 * Startup menu with a 2-tall card grid, game settings pages,
 * and player management.
 */

#include "games/main_menu.hpp"
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

static constexpr float    WINDOW_W      = 1280.0f;
static constexpr float    WINDOW_H      = 720.0f;

// Card grid
static constexpr float    LEFT_MARGIN   = 40.0f;
static constexpr float    TOP_MARGIN    = 80.0f;    // space for title
static constexpr float    CARD_W        = 360.0f;
static constexpr float    CARD_H        = 220.0f;
static constexpr float    CARD_GAP      = 20.0f;
static constexpr int      VISIBLE_COLS  = 3;
static constexpr uint32_t CARD_Z        = 10;

// Font sizes
static constexpr float    TITLE_PT      = 48.0f;
static constexpr float    CARD_PT       = 28.0f;
static constexpr float    SMALL_PT      = 26.0f;

// Settings sub-screen
static constexpr float    SETTINGS_LEFT = 100.0f;
static constexpr float    SETTINGS_TOP  = 100.0f;
static constexpr float    ROW_HEIGHT    = 55.0f;
static constexpr float    ROW_PAD       = 12.0f;
static constexpr float    OPTION_X      = 600.0f;
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
    , m_renaming(false)
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
    if(m_state != MenuState::PlayerSettings || !m_renaming || text == nullptr)
    {
        return;
    }

    // Route to virtual keyboard if it's open
    if(m_virtualKeyboard.isOpen())
    {
        m_virtualKeyboard.handleTextInput(text);
        return;
    }

    // Append text to rename buffer (may be multi-byte UTF-8)
    std::string input(text);
    if(m_renameBuffer.length() + input.length() <= MAX_NAME_LEN)
    {
        m_renameBuffer += input;
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
        m_renaming = false;
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
        text->m_y        = 15.0f;
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
                accent->m_width  = 5.0f;
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
            titleText->m_x        = cardX + 20.0f;
            titleText->m_y        = cardY + 30.0f;
            titleText->m_z        = CARD_Z + 2;
            renderQueueAdd(fid, titleText);

            // Card description
            auto descText = std::make_shared<RenderText>();
            descText->m_text     = cardDesc;
            descText->m_color    = {160, 160, 170};
            descText->m_fontId   = m_smallFontId;
            descText->m_rotation = 0.0f;
            descText->m_scaleX   = 1.0f;
            descText->m_scaleY   = 1.0f;
            descText->m_x        = cardX + 20.0f;
            descText->m_y        = cardY + 70.0f;
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
                infoText->m_x        = cardX + 20.0f;
                infoText->m_y        = cardY + CARD_H - 40.0f;
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
        arrow->m_x        = 8.0f;
        arrow->m_y        = TOP_MARGIN + CARD_H - 10.0f;
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
        arrow->m_x        = WINDOW_W - 35.0f;
        arrow->m_y        = TOP_MARGIN + CARD_H - 10.0f;
        arrow->m_z        = CARD_Z + 3;
        renderQueueAdd(fid, arrow);
    }

    // Navigation hints (above the game bar at y=620)
    m_inputHints.render(fid, m_smallFontId, LEFT_MARGIN, 575.0f, CARD_Z, {
        {SDLK_UP,     GAMEPAD_ICON_LEFT_STICK,       "navigate"},
        {SDLK_RETURN, SDL_GAMEPAD_BUTTON_SOUTH,      "select"}
    });
}


// ============================================================================
// Game Settings sub-screen
// ============================================================================

void MainMenu::handleGameSettingsKey(uint32_t keycode)
{
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
        text->m_y        = 30.0f;
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
        text->m_y        = 85.0f;
        text->m_z        = SETTINGS_Z;
        renderQueueAdd(fid, text);
    }

    // Settings rows
    for(int i = 0; i < settingCount; i++)
    {
        float rowY = SETTINGS_TOP + 30.0f + i * ROW_HEIGHT;
        bool isSelected = (i == m_settingsCursor);

        // Row highlight
        if(isSelected)
        {
            auto bg = std::make_shared<RenderShape>();
            bg->m_type   = ShapeType::Box;
            bg->m_color  = {60, 60, 100};
            bg->m_x      = SETTINGS_LEFT - 10.0f;
            bg->m_y      = rowY - 5.0f;
            bg->m_z      = SETTINGS_Z;
            bg->m_width  = WINDOW_W - 2 * SETTINGS_LEFT + 20.0f;
            bg->m_height = ROW_HEIGHT - 4.0f;
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
        float btnY = SETTINGS_TOP + 30.0f + settingCount * ROW_HEIGHT;
        bool isSelected = (m_settingsCursor == settingCount);

        if(isSelected)
        {
            auto bg = std::make_shared<RenderShape>();
            bg->m_type   = ShapeType::Box;
            bg->m_color  = {40, 100, 60};
            bg->m_x      = SETTINGS_LEFT - 10.0f;
            bg->m_y      = btnY - 5.0f;
            bg->m_z      = SETTINGS_Z;
            bg->m_width  = 250.0f;
            bg->m_height = ROW_HEIGHT - 4.0f;
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
    m_inputHints.render(fid, m_smallFontId, SETTINGS_LEFT, 575.0f, SETTINGS_Z, {
        {SDLK_UP,     GAMEPAD_ICON_LEFT_STICK,       "navigate"},
        {SDLK_LEFT,   GAMEPAD_ICON_LEFT_STICK,       "change"},
        {SDLK_RETURN, SDL_GAMEPAD_BUTTON_SOUTH,      "confirm"},
        {SDLK_ESCAPE, SDL_GAMEPAD_BUTTON_EAST,       "back"}
    });
}


// ============================================================================
// Player Settings sub-screen
// ============================================================================

void MainMenu::handlePlayerSettingsKey(uint32_t keycode)
{
    uint8_t playerCount = getPlayerCount();
    int totalRows = playerCount + 1;  // players + "Add Player" row

    // If virtual keyboard is open, route input there
    if(m_virtualKeyboard.isOpen())
    {
        VirtualKeyboardResult result = m_virtualKeyboard.handleKey(keycode);
        if(result == VirtualKeyboardResult::Confirmed)
        {
            PlayerID pid = getPlayerByIndex(static_cast<uint8_t>(m_playerCursor));
            if(pid != INVALID_PLAYER_ID)
            {
                setPlayerName(pid, m_virtualKeyboard.getText());
            }
            m_renaming = false;
            m_virtualKeyboard.close();
        }
        else if(result == VirtualKeyboardResult::Cancelled)
        {
            m_renaming = false;
            m_virtualKeyboard.close();
        }
        return;
    }

    if(m_renaming)
    {
        // In rename mode: handle text editing keys (physical keyboard)
        switch(keycode)
        {
            case SDLK_RETURN:
            case SDLK_KP_ENTER:
            {
                // Confirm rename
                if(!m_renameBuffer.empty())
                {
                    PlayerID pid = getPlayerByIndex(static_cast<uint8_t>(m_playerCursor));
                    if(pid != INVALID_PLAYER_ID)
                    {
                        setPlayerName(pid, m_renameBuffer);
                    }
                }
                m_renaming = false;
                SDL_StopTextInput(SDL_GetKeyboardFocus());
                break;
            }
            case SDLK_ESCAPE:
                // Cancel rename
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

    switch(keycode)
    {
        case SDLK_UP:
            m_playerCursor--;
            if(m_playerCursor < 0) m_playerCursor = totalRows - 1;
            break;
        case SDLK_DOWN:
            m_playerCursor++;
            if(m_playerCursor >= totalRows) m_playerCursor = 0;
            break;
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
            if(m_playerCursor < playerCount)
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
            else
            {
                // Add Player
                std::string newName = "Player " + std::to_string(playerCount + 1);
                createPlayer(newName);
            }
            break;
        case SDLK_DELETE:
        case SDLK_BACKSPACE:
            if(m_playerCursor < playerCount)
            {
                PlayerID pid = getPlayerByIndex(static_cast<uint8_t>(m_playerCursor));
                if(pid != INVALID_PLAYER_ID)
                {
                    removePlayer(pid);
                    // Adjust cursor if needed
                    uint8_t newCount = getPlayerCount();
                    int newTotal = newCount + 1;
                    if(m_playerCursor >= newTotal)
                    {
                        m_playerCursor = newTotal - 1;
                    }
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


void MainMenu::renderPlayerSettings()
{
    FrameID fid = getFrameId();
    uint8_t playerCount = getPlayerCount();

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
        text->m_y        = 30.0f;
        text->m_z        = SETTINGS_Z;
        renderQueueAdd(fid, text);
    }

    // Player rows
    for(uint8_t i = 0; i < playerCount; i++)
    {
        float rowY = SETTINGS_TOP + 10.0f + i * ROW_HEIGHT;
        bool isSelected = (static_cast<int>(i) == m_playerCursor);

        // Row highlight
        if(isSelected)
        {
            auto bg = std::make_shared<RenderShape>();
            bg->m_type   = ShapeType::Box;
            bg->m_color  = {60, 60, 100};
            bg->m_x      = SETTINGS_LEFT - 10.0f;
            bg->m_y      = rowY - 5.0f;
            bg->m_z      = SETTINGS_Z;
            bg->m_width  = WINDOW_W - 2 * SETTINGS_LEFT + 20.0f;
            bg->m_height = ROW_HEIGHT - 4.0f;
            renderQueueAdd(fid, bg);
        }

        PlayerID pid = getPlayerByIndex(i);
        std::string name;
        if(m_renaming && isSelected)
        {
            if(m_virtualKeyboard.isOpen())
            {
                // Virtual keyboard manages its own text display
                name = m_virtualKeyboard.getText() + "_";
            }
            else
            {
                // Show the rename buffer with a cursor
                name = m_renameBuffer + "_";
            }
        }
        else
        {
            name = (pid != INVALID_PLAYER_ID) ? getPlayerName(pid) : "???";
        }

        // Center text vertically within the highlight box
        float boxY      = rowY - 5.0f;
        float boxH      = ROW_HEIGHT - 4.0f;
        int textW = 0, textH = 0;
        TTF_Font* font = getFont(m_cardFontId);
        if(font)
        {
            TTF_GetStringSize(font, name.c_str(), 0, &textW, &textH);
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

        // Action hints for selected row (not in rename mode)
        if(isSelected && !m_renaming)
        {
            float hintsY = boxY + (boxH - 36.0f) * 0.5f;  // center 36px icons in box
            m_inputHints.render(fid, m_smallFontId, OPTION_X, hintsY, SETTINGS_Z, {
                {SDLK_RETURN, SDL_GAMEPAD_BUTTON_SOUTH, "rename"},
                {SDLK_DELETE, SDL_GAMEPAD_BUTTON_WEST,  "remove"}
            });
        }
    }

    // "Add Player" row
    {
        float rowY = SETTINGS_TOP + 10.0f + playerCount * ROW_HEIGHT;
        bool isSelected = (m_playerCursor == playerCount);
        float addBoxY = rowY - 5.0f;
        float addBoxH = ROW_HEIGHT - 4.0f;

        if(isSelected)
        {
            auto bg = std::make_shared<RenderShape>();
            bg->m_type   = ShapeType::Box;
            bg->m_color  = {40, 80, 60};
            bg->m_x      = SETTINGS_LEFT - 10.0f;
            bg->m_y      = addBoxY;
            bg->m_z      = SETTINGS_Z;
            bg->m_width  = 300.0f;
            bg->m_height = addBoxH;
            renderQueueAdd(fid, bg);
        }

        std::string addLabel = "+ Add Player";
        int addTextW = 0, addTextH = 0;
        TTF_Font* addFont = getFont(m_cardFontId);
        if(addFont)
        {
            TTF_GetStringSize(addFont, addLabel.c_str(), 0, &addTextW, &addTextH);
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

    // Virtual keyboard overlay (renders on top of everything)
    if(m_virtualKeyboard.isOpen())
    {
        m_virtualKeyboard.render(fid, SETTINGS_Z + 90);
    }
    else if(m_renaming)
    {
        // Bottom hints for physical keyboard rename mode
        m_inputHints.render(fid, m_smallFontId, SETTINGS_LEFT, 575.0f, SETTINGS_Z, {
            {SDLK_RETURN, SDL_GAMEPAD_BUTTON_SOUTH, "confirm"},
            {SDLK_ESCAPE, SDL_GAMEPAD_BUTTON_EAST,  "cancel"}
        });
    }
    else
    {
        m_inputHints.render(fid, m_smallFontId, SETTINGS_LEFT, 575.0f, SETTINGS_Z, {
            {SDLK_UP,     GAMEPAD_ICON_LEFT_STICK,    "navigate"},
            {SDLK_RETURN, SDL_GAMEPAD_BUTTON_SOUTH,   "select"},
            {SDLK_ESCAPE, SDL_GAMEPAD_BUTTON_EAST,    "back"}
        });
    }
}
