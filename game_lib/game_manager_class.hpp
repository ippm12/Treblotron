/**
 * game_manager_class.hpp
 *
 * Internal GameManager class definition. This header is private
 * to the game_lib module and should not be included by other modules.
 */

#ifndef GAME_MANAGER_CLASS_HPP
#define GAME_MANAGER_CLASS_HPP

#include <cstdint>
#include <functional>
#include "common_inc.hpp"
#include "frame/frame.hpp"
#include "game_lib/game.hpp"
#include "game_lib/input_hints.hpp"

class GameManager
{
    public:
        GameManager();
        ~GameManager();

        /**
         * Creates internal state, opens the main window, and starts
         * the delta time clock.
         */
        Status initialize();

        /**
         * Unloads the current game, closes the window, and tears
         * down the manager.
         */
        void shutdown();

        /**
         * Loads a game. Unloads any currently active game first,
         * passes the frame to the new game, then calls init.
         */
        Status loadGame(GamePtr game, std::function<GamePtr()> restartFactory = nullptr);

        /**
         * Unloads the current game by calling its shutdown.
         */
        Status unloadGame();

        Status restartCurrentGame();

        /**
         * Runs one frame: computes delta time (clamped to 0.25s),
         * calls update, then clears/renders/flushes/presents.
         */
        void tick();

    private:
        /** Enqueue the status bar elements for the current frame. */
        void enqueueBar(const GameBarInfo& info);

        /** Render the pause menu overlay. */
        void renderPauseMenu();

        /** Handle a key press while the pause menu is open. */
        void handlePauseKey(uint32_t keycode);

        bool m_initialized;
        GamePtr m_currentGame;
        uint64_t m_lastTickNs;
        FrameID m_frameId;
        FontID m_barFontId;
        FontID m_pauseFontId;

        // Pause menu state
        bool    m_paused = false;
        uint8_t m_pauseCursor = 0; // 0 = Resume, 1 = Restart, 2 = Main Menu

        // Factory to recreate current game for restart (nullopt = no restart)
        std::function<GamePtr()> m_gameFactory;

        // Input hints for pause instruction in the status bar
        InputHints m_inputHints;

#ifdef DARTLENS_SHOW_FPS
        void enqueueFps(float deltaTime);
        FontID   m_fpsFontId;
        float    m_fpsAccumulator;
        int      m_fpsFrameCount;
        int      m_fpsDisplay;
#endif
};

#endif // GAME_MANAGER_CLASS_HPP
