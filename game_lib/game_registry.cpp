/**
 * game_registry.cpp
 *
 * Implementation of the game descriptor registry.
 */

#include "game_lib/game_registry.hpp"


static std::vector<GameDescriptor>& getRegistry()
{
    static std::vector<GameDescriptor> s_registry;
    return s_registry;
}


void registerGame(const GameDescriptor& descriptor)
{
    getRegistry().push_back(descriptor);
}


const std::vector<GameDescriptor>& getRegisteredGames()
{
    return getRegistry();
}
