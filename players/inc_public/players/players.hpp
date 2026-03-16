/**
 * players.hpp
 *
 * Players module: manages a list of active players and teams.
 * Other modules use these free functions to create, remove,
 * and query player and team information.
 */

#ifndef PLAYERS_HPP
#define PLAYERS_HPP

#include "common_inc.hpp"
#include <cstdint>
#include <string>
#include <vector>

// ============================================================================
// Player types and constants
// ============================================================================

#define INVALID_PLAYER_ID   SIZE_MAX
typedef size_t PlayerID;

/** Maximum number of players the module can track. */
#define MAX_NUM_PLAYERS     16

// ============================================================================
// Team types and constants
// ============================================================================

#define INVALID_TEAM_ID     SIZE_MAX
typedef size_t TeamID;

/** Maximum number of teams the module can track. */
#define MAX_NUM_TEAMS       4

struct Team
{
    std::string name;
};

// ============================================================================
// Player struct
// ============================================================================

struct Player
{
    std::string name;
    TeamID      teamId = INVALID_TEAM_ID;
};

// ============================================================================
// Player functions
// ============================================================================

/** Initialize the players module. Must be called before any other player function. */
Status initializePlayersModule();

/** Shut down the players module and clear all player data. */
void shutdownPlayersModule();

/** Create a new player with the given name. Returns the PlayerID, or INVALID_PLAYER_ID on failure.
 *  If teams exist, the new player is auto-assigned to the team with fewest members. */
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

// ============================================================================
// Team functions
// ============================================================================

/** Create a new team with the given name. Returns the TeamID, or INVALID_TEAM_ID on failure.
 *  If this is the first team, all existing players are auto-assigned to it. */
TeamID createTeam(const std::string& name);

/** Remove a team by ID. Players on this team are reassigned to the first remaining team.
 *  If this was the last team, all players revert to INVALID_TEAM_ID. */
Status removeTeam(TeamID id);

/** Get the number of active teams. */
uint8_t getTeamCount();

/** Get a team's name by ID. Returns empty string if invalid. */
const std::string& getTeamName(TeamID id);

/** Set a team's name by ID. */
Status setTeamName(TeamID id, const std::string& name);

/** Get the TeamID of the Nth active team (0-based index). Returns INVALID_TEAM_ID if out of range. */
TeamID getTeamByIndex(uint8_t index);

/** Assign a player to a team. The team must be active. */
Status setPlayerTeam(PlayerID playerId, TeamID teamId);

/** Get the team a player belongs to. Returns INVALID_TEAM_ID if unassigned or invalid. */
TeamID getPlayerTeam(PlayerID playerId);

/** Get the player indices (0-based, as used by getPlayerByIndex) for all players on a team. */
std::vector<uint8_t> getPlayerIndicesForTeam(TeamID teamId);

#endif // PLAYERS_HPP
