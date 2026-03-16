/**
 * players.cpp
 *
 * Players and teams module implementation.
 */

#include "players/players.hpp"

static bool   f_initialized = false;
static Player f_players[MAX_NUM_PLAYERS];
static bool   f_active[MAX_NUM_PLAYERS];
static Team   f_teams[MAX_NUM_TEAMS];
static bool   f_teamActive[MAX_NUM_TEAMS];


// ============================================================================
// Internal helpers
// ============================================================================

/** Find the first active team ID, or INVALID_TEAM_ID if none. */
static TeamID findFirstActiveTeam()
{
    for(size_t i = 0; i < MAX_NUM_TEAMS; i++)
    {
        if(f_teamActive[i])
        {
            return i;
        }
    }
    return INVALID_TEAM_ID;
}


/** Find the active team with the fewest players assigned. */
static TeamID findSmallestTeam()
{
    TeamID bestId = INVALID_TEAM_ID;
    int bestCount = INT32_MAX;

    for(size_t t = 0; t < MAX_NUM_TEAMS; t++)
    {
        if(!f_teamActive[t]) continue;

        int count = 0;
        for(size_t p = 0; p < MAX_NUM_PLAYERS; p++)
        {
            if(f_active[p] && f_players[p].teamId == t)
            {
                count++;
            }
        }

        if(count < bestCount)
        {
            bestCount = count;
            bestId = t;
        }
    }

    return bestId;
}


// ============================================================================
// Module lifecycle
// ============================================================================

Status initializePlayersModule()
{
    if(f_initialized)
    {
        LOG_ERROR(GAME_MANAGER_LOG_ID, "Players module already initialized");
        return STATUS_ERROR_INVALID_STATE;
    }

    for(size_t i = 0; i < MAX_NUM_PLAYERS; i++)
    {
        f_players[i].name.clear();
        f_players[i].teamId = INVALID_TEAM_ID;
        f_active[i] = false;
    }

    for(size_t i = 0; i < MAX_NUM_TEAMS; i++)
    {
        f_teams[i].name.clear();
        f_teamActive[i] = false;
    }

    f_initialized = true;
    LOG_INFO(GAME_MANAGER_LOG_ID, "Players module initialized");
    return STATUS_OK;
}


void shutdownPlayersModule()
{
    if(!f_initialized)
    {
        return;
    }

    for(size_t i = 0; i < MAX_NUM_PLAYERS; i++)
    {
        f_players[i].name.clear();
        f_players[i].teamId = INVALID_TEAM_ID;
        f_active[i] = false;
    }

    for(size_t i = 0; i < MAX_NUM_TEAMS; i++)
    {
        f_teams[i].name.clear();
        f_teamActive[i] = false;
    }

    f_initialized = false;
    LOG_INFO(GAME_MANAGER_LOG_ID, "Players module shut down");
}


// ============================================================================
// Player functions
// ============================================================================

PlayerID createPlayer(const std::string& name)
{
    if(!f_initialized)
    {
        LOG_ERROR(GAME_MANAGER_LOG_ID, "Players module not initialized");
        return INVALID_PLAYER_ID;
    }

    for(size_t i = 0; i < MAX_NUM_PLAYERS; i++)
    {
        if(!f_active[i])
        {
            f_players[i].name = name;
            f_active[i] = true;

            // Auto-assign to smallest team if teams exist
            if(getTeamCount() > 0)
            {
                f_players[i].teamId = findSmallestTeam();
            }
            else
            {
                f_players[i].teamId = INVALID_TEAM_ID;
            }

            LOG_INFO(GAME_MANAGER_LOG_ID, "Created player {} (ID {})", name, i);
            return i;
        }
    }

    LOG_ERROR(GAME_MANAGER_LOG_ID, "Cannot create player '{}': max players reached", name);
    return INVALID_PLAYER_ID;
}


Status removePlayer(PlayerID id)
{
    if(!f_initialized)
    {
        LOG_ERROR(GAME_MANAGER_LOG_ID, "Players module not initialized");
        return STATUS_ERROR_NOT_INIT;
    }

    if(id >= MAX_NUM_PLAYERS || !f_active[id])
    {
        LOG_ERROR(GAME_MANAGER_LOG_ID, "Invalid player ID: {}", id);
        return STATUS_ERROR_INVALID_PARAM;
    }

    LOG_INFO(GAME_MANAGER_LOG_ID, "Removed player {} (ID {})", f_players[id].name, id);
    f_players[id].name.clear();
    f_players[id].teamId = INVALID_TEAM_ID;
    f_active[id] = false;
    return STATUS_OK;
}


uint8_t getPlayerCount()
{
    uint8_t count = 0;
    for(size_t i = 0; i < MAX_NUM_PLAYERS; i++)
    {
        if(f_active[i])
        {
            count++;
        }
    }
    return count;
}


const std::string& getPlayerName(PlayerID id)
{
    static const std::string empty;

    if(!f_initialized || id >= MAX_NUM_PLAYERS || !f_active[id])
    {
        return empty;
    }

    return f_players[id].name;
}


Status setPlayerName(PlayerID id, const std::string& name)
{
    if(!f_initialized)
    {
        LOG_ERROR(GAME_MANAGER_LOG_ID, "Players module not initialized");
        return STATUS_ERROR_NOT_INIT;
    }

    if(id >= MAX_NUM_PLAYERS || !f_active[id])
    {
        LOG_ERROR(GAME_MANAGER_LOG_ID, "Invalid player ID: {}", id);
        return STATUS_ERROR_INVALID_PARAM;
    }

    f_players[id].name = name;
    return STATUS_OK;
}


PlayerID getPlayerByIndex(uint8_t index)
{
    if(!f_initialized)
    {
        return INVALID_PLAYER_ID;
    }

    uint8_t count = 0;
    for(size_t i = 0; i < MAX_NUM_PLAYERS; i++)
    {
        if(f_active[i])
        {
            if(count == index)
            {
                return i;
            }
            count++;
        }
    }

    return INVALID_PLAYER_ID;
}


// ============================================================================
// Team functions
// ============================================================================

TeamID createTeam(const std::string& name)
{
    if(!f_initialized)
    {
        LOG_ERROR(GAME_MANAGER_LOG_ID, "Players module not initialized");
        return INVALID_TEAM_ID;
    }

    for(size_t i = 0; i < MAX_NUM_TEAMS; i++)
    {
        if(!f_teamActive[i])
        {
            bool firstTeam = (getTeamCount() == 0);

            f_teams[i].name = name;
            f_teamActive[i] = true;
            LOG_INFO(GAME_MANAGER_LOG_ID, "Created team {} (ID {})", name, i);

            // If this is the first team, auto-assign all existing players to it
            if(firstTeam)
            {
                for(size_t p = 0; p < MAX_NUM_PLAYERS; p++)
                {
                    if(f_active[p])
                    {
                        f_players[p].teamId = i;
                    }
                }
            }

            return i;
        }
    }

    LOG_ERROR(GAME_MANAGER_LOG_ID, "Cannot create team '{}': max teams reached", name);
    return INVALID_TEAM_ID;
}


Status removeTeam(TeamID id)
{
    if(!f_initialized)
    {
        LOG_ERROR(GAME_MANAGER_LOG_ID, "Players module not initialized");
        return STATUS_ERROR_NOT_INIT;
    }

    if(id >= MAX_NUM_TEAMS || !f_teamActive[id])
    {
        LOG_ERROR(GAME_MANAGER_LOG_ID, "Invalid team ID: {}", id);
        return STATUS_ERROR_INVALID_PARAM;
    }

    LOG_INFO(GAME_MANAGER_LOG_ID, "Removed team {} (ID {})", f_teams[id].name, id);
    f_teams[id].name.clear();
    f_teamActive[id] = false;

    // Check if any teams remain
    TeamID fallback = findFirstActiveTeam();

    // Reassign players that were on this team
    for(size_t p = 0; p < MAX_NUM_PLAYERS; p++)
    {
        if(f_active[p] && f_players[p].teamId == id)
        {
            f_players[p].teamId = fallback; // INVALID_TEAM_ID if no teams left
        }
    }

    return STATUS_OK;
}


uint8_t getTeamCount()
{
    uint8_t count = 0;
    for(size_t i = 0; i < MAX_NUM_TEAMS; i++)
    {
        if(f_teamActive[i])
        {
            count++;
        }
    }
    return count;
}


const std::string& getTeamName(TeamID id)
{
    static const std::string empty;

    if(!f_initialized || id >= MAX_NUM_TEAMS || !f_teamActive[id])
    {
        return empty;
    }

    return f_teams[id].name;
}


Status setTeamName(TeamID id, const std::string& name)
{
    if(!f_initialized)
    {
        LOG_ERROR(GAME_MANAGER_LOG_ID, "Players module not initialized");
        return STATUS_ERROR_NOT_INIT;
    }

    if(id >= MAX_NUM_TEAMS || !f_teamActive[id])
    {
        LOG_ERROR(GAME_MANAGER_LOG_ID, "Invalid team ID: {}", id);
        return STATUS_ERROR_INVALID_PARAM;
    }

    f_teams[id].name = name;
    return STATUS_OK;
}


TeamID getTeamByIndex(uint8_t index)
{
    if(!f_initialized)
    {
        return INVALID_TEAM_ID;
    }

    uint8_t count = 0;
    for(size_t i = 0; i < MAX_NUM_TEAMS; i++)
    {
        if(f_teamActive[i])
        {
            if(count == index)
            {
                return i;
            }
            count++;
        }
    }

    return INVALID_TEAM_ID;
}


Status setPlayerTeam(PlayerID playerId, TeamID teamId)
{
    if(!f_initialized)
    {
        LOG_ERROR(GAME_MANAGER_LOG_ID, "Players module not initialized");
        return STATUS_ERROR_NOT_INIT;
    }

    if(playerId >= MAX_NUM_PLAYERS || !f_active[playerId])
    {
        LOG_ERROR(GAME_MANAGER_LOG_ID, "Invalid player ID: {}", playerId);
        return STATUS_ERROR_INVALID_PARAM;
    }

    if(teamId >= MAX_NUM_TEAMS || !f_teamActive[teamId])
    {
        LOG_ERROR(GAME_MANAGER_LOG_ID, "Invalid team ID: {}", teamId);
        return STATUS_ERROR_INVALID_PARAM;
    }

    f_players[playerId].teamId = teamId;
    return STATUS_OK;
}


TeamID getPlayerTeam(PlayerID playerId)
{
    if(!f_initialized || playerId >= MAX_NUM_PLAYERS || !f_active[playerId])
    {
        return INVALID_TEAM_ID;
    }

    return f_players[playerId].teamId;
}


std::vector<uint8_t> getPlayerIndicesForTeam(TeamID teamId)
{
    std::vector<uint8_t> indices;

    if(!f_initialized || teamId >= MAX_NUM_TEAMS || !f_teamActive[teamId])
    {
        return indices;
    }

    uint8_t index = 0;
    for(size_t i = 0; i < MAX_NUM_PLAYERS; i++)
    {
        if(f_active[i])
        {
            if(f_players[i].teamId == teamId)
            {
                indices.push_back(index);
            }
            index++;
        }
    }

    return indices;
}
