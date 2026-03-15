/**
 * players.cpp
 *
 * Players module implementation.
 */

#include "players/players.hpp"

static bool   f_initialized = false;
static Player f_players[MAX_NUM_PLAYERS];
static bool   f_active[MAX_NUM_PLAYERS];


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
        f_active[i] = false;
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
        f_active[i] = false;
    }

    f_initialized = false;
    LOG_INFO(GAME_MANAGER_LOG_ID, "Players module shut down");
}


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
