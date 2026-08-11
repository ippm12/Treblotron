/**
 * game_manager.hpp
 *
 * Public API for the game manager module.
 */

#ifndef GAME_MANAGER_HPP
#define GAME_MANAGER_HPP

#include <functional>
#include "common_inc.hpp"
#include "game_lib/game.hpp"

/**
 * Initializes the game manager module.
 * Must be called after initializeFrameModule().
 */
Status initializeGameManager();

/**
 * Shuts down the game manager module. Unloads current game.
 */
void shutdownGameManager();

/**
 * Load a game. If a game is currently active, it is unloaded first.
 * Optionally pass a factory function to enable restarting the game
 * from the pause menu.
 */
Status loadGame(GamePtr game, std::function<GamePtr()> restartFactory = nullptr);

/**
 * Unload the current game.
 */
Status unloadGame();

/**
 * Restart the current game using the stored factory.
 * Returns STATUS_ERROR_INVALID_STATE if no factory was provided at load time.
 */
Status restartCurrentGame();

/**
 * Run one tick of the game loop: compute delta time
 * and call the current game's update.
 */
void tickGameManager();

/**
 * Open the vision settings overlay: server address and detection thresholds.
 *
 * An overlay owned by the game manager rather than a screen, so it can be
 * raised over whatever is running without disturbing it — a threshold that is
 * mis-scoring is best adjusted during the leg that revealed it, not after
 * abandoning it. F1 (or Back on a controller) does the same thing in-game;
 * this is the main menu's way in.
 */
void openVisionSettings();

#endif // GAME_MANAGER_HPP
