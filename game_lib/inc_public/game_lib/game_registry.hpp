/**
 * game_registry.hpp
 *
 * Registry for game descriptors. Games register their settings and factory
 * functions here so the menu can display them without instantiating games.
 */

#ifndef GAME_REGISTRY_HPP
#define GAME_REGISTRY_HPP

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class Game;
typedef std::shared_ptr<Game> GamePtr;

/** A single option within a game setting (e.g. "301", "501"). */
struct GameSettingOption
{
    std::string label;
};

/** A configurable setting for a game (e.g. "Starting Score"). */
struct GameSetting
{
    std::string name;
    std::vector<GameSettingOption> options;
    size_t defaultIndex = 0;
};

/** Describes a game type: its name, settings, and how to create an instance. */
struct GameDescriptor
{
    std::string name;
    std::string description;
    uint8_t     maxPlayers = 1;
    std::vector<GameSetting> settings;

    /** Factory: given the chosen index for each setting, create a Game instance. */
    std::function<GamePtr(const std::vector<size_t>&)> createGame;
};

/** Register a game descriptor. Call at startup before the menu is loaded. */
void registerGame(const GameDescriptor& descriptor);

/** Get all registered game descriptors. */
const std::vector<GameDescriptor>& getRegisteredGames();

#endif // GAME_REGISTRY_HPP
