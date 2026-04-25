/**
 * main_menu.hpp
 *
 * Startup menu screen with a 2-tall card grid that scrolls horizontally.
 * Supports game selection with per-game settings, and player management.
 */

#ifndef MAIN_MENU_HPP
#define MAIN_MENU_HPP

#include "game_lib/game.hpp"
#include "game_lib/input_hints.hpp"
#include "game_lib/virtual_keyboard.hpp"
#include <functional>
#include <string>
#include <vector>

enum class MenuState
{
    CardGrid,
    GameSettings,
    PlayerSettings
};

enum class CardType
{
    PlayerSettings,
    Calibration,
    VisionDebug,
    Game,
    Exit
};

struct MenuCard
{
    CardType type;
    size_t   gameIndex;   // index into getRegisteredGames(), only valid for CardType::Game
};

class MainMenu : public Game
{
    public:
        MainMenu();
        ~MainMenu() override = default;

        Status init(FrameID frameId) override;
        void update(float deltaTime) override;
        void render() override;
        void shutdown() override;
        uint8_t getMaxPlayers() const override;
        bool isPauseable() const override;

        // Input callbacks from GameManager
        void onKeyDown(uint32_t keycode) override;
        void onGamepadButton(uint8_t button, bool pressed) override;
        void onTextInput(const char* text) override;

    private:
        // State machine
        MenuState m_state;

        // Card grid
        std::vector<MenuCard> m_cards;
        int m_cursorCol;
        int m_cursorRow;
        int m_scrollOffset;
        int m_totalCols;

        // Game settings sub-screen
        size_t              m_selectedGameIndex;
        std::vector<size_t> m_settingChoices;
        int                 m_settingsCursor;  // 0..N-1 = settings, N = Start button

        // Player settings sub-screen
        int         m_playerCursor;         // unified cursor across players + teams sections
        int         m_playerSettingsScroll;  // first visible row for scrolling
        bool        m_renaming;
        std::string m_renameBuffer;
        bool        m_renamingTeam;
        std::string m_teamRenameBuffer;
        bool        m_showEmptyTeamWarning;
        bool        m_showDuplicateNameWarning;
        bool        m_showNoTeamsWarning;
        bool        m_showNoPlayersWarning;
        bool        m_showSimWarning;

        // Fonts
        FontID m_titleFontId;
        FontID m_cardFontId;
        FontID m_smallFontId;

        // Per-state key handlers
        void handleCardGridKey(uint32_t keycode);
        void handleGameSettingsKey(uint32_t keycode);
        void handlePlayerSettingsKey(uint32_t keycode);

        // Per-state renderers
        void renderCardGrid();
        void renderGameSettings();
        void renderPlayerSettings();

        // Helpers
        int  cardIndexAt(int col, int row) const;
        int  columnCount() const;
        void openCard();
        void clampCursor();

        // Input hints
        InputHints m_inputHints;

        // Virtual keyboard for controller text input
        VirtualKeyboard m_virtualKeyboard;
};

#endif // MAIN_MENU_HPP
