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
#include "vision/vision_source.hpp"

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
 * Set the vision source. Initializes it and connects to the current game.
 */
Status setVisionSource(VisionSourcePtr source);

/**
 * Returns true if the vision source reports no darts on the board.
 * Returns true if no vision source is set.
 */
bool isBoardClear();

/**
 * Run one tick of the game loop: compute delta time
 * and call the current game's update.
 */
void tickGameManager();

#endif // GAME_MANAGER_HPP
