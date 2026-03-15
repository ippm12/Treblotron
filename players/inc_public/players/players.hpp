/**
 * players.hpp
 *
 * Players module: manages a list of active players.
 * Other modules use these free functions to create, remove,
 * and query player information.
 */

#ifndef PLAYERS_HPP
#define PLAYERS_HPP

#include "common_inc.hpp"
#include <string>

#define INVALID_PLAYER_ID   SIZE_MAX
typedef size_t PlayerID;

/** Maximum number of players the module can track. */
#define MAX_NUM_PLAYERS     16

struct Player
{
    std::string name;
};

/** Initialize the players module. Must be called before any other player function. */
Status initializePlayersModule();

/** Shut down the players module and clear all player data. */
void shutdownPlayersModule();

/** Create a new player with the given name. Returns the PlayerID, or INVALID_PLAYER_ID on failure. */
PlayerID createPlayer(const std::string& name);

/** Remove a player by ID. */
Status removePlayer(PlayerID id);

/** Get the number of active players. */
uint8_t getPlayerCount();

/** Get a player's name by ID. Returns empty string if invalid. */
const std::string& getPlayerName(PlayerID id);

/** Set a player's name by ID. */
Status setPlayerName(PlayerID id, const std::string& name);

/** Get the PlayerID of the Nth active player (0-based index). Returns INVALID_PLAYER_ID if out of range. */
PlayerID getPlayerByIndex(uint8_t index);

#endif // PLAYERS_HPP
