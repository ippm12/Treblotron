/**
 * startup.cpp
 *
 * The main function that runbs on startup
 */

#include <memory>
#include "common_inc.hpp"
#include "frame/frame.hpp"
#include "players/players.hpp"
#include "game_lib/game_manager.hpp"
#include "games/main_menu.hpp"

#ifdef DARTLENS_USE_SIM
#include "vision/sim_vision_source.hpp"
#endif

int main()
{
    Status stat = initializeLoggingModule(LOGGING_LEVEL_INFO);
    if(IS_STATUS_NOT_OK(stat))
    {
        return -1;
    }

    stat = initializeFrameModule();
    if(IS_STATUS_NOT_OK(stat))
    {
        LOG_CRITICAL(MAIN_LOG_ID, "Failed Frame Module Init");
        shutdownLoggingModule();
        return -1;
    }

    stat = initializeGameManager();
    if(IS_STATUS_NOT_OK(stat))
    {
        LOG_CRITICAL(MAIN_LOG_ID, "Failed Game Manager Init");
        shutdownFrameModule();
        shutdownLoggingModule();
        return -1;
    }

    stat = initializePlayersModule();
    if(IS_STATUS_NOT_OK(stat))
    {
        LOG_CRITICAL(MAIN_LOG_ID, "Failed Players Module Init");
        shutdownGameManager();
        shutdownFrameModule();
        shutdownLoggingModule();
        return -1;
    }

    // Load main menu (games auto-register via static initializers)
    stat = loadGame(std::make_shared<MainMenu>());
    if(IS_STATUS_NOT_OK(stat))
    {
        LOG_CRITICAL(MAIN_LOG_ID, "Failed to load main menu");
        shutdownPlayersModule();
        shutdownGameManager();
        shutdownFrameModule();
        shutdownLoggingModule();
        return -1;
    }

    // Set up vision source
#ifdef DARTLENS_USE_SIM
    stat = setVisionSource(std::make_shared<SimVisionSource>());
    if(IS_STATUS_NOT_OK(stat))
    {
        LOG_CRITICAL(MAIN_LOG_ID, "Failed to initialize sim vision source");
        shutdownPlayersModule();
        shutdownGameManager();
        shutdownFrameModule();
        shutdownLoggingModule();
        return -1;
    }
#endif

    LOG_INFO(MAIN_LOG_ID, "Finished Initializing");

    while(pollFrames())
    {
        tickGameManager();
    }

    shutdownPlayersModule();
    shutdownGameManager();
    shutdownFrameModule();
    shutdownLoggingModule();
    return 0;
}
