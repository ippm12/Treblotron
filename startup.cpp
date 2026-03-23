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
#include "vision/vision.hpp"
#include "games/main_menu.hpp"

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

    // Initialize vision module (creates sim or real source based on build config)
    stat = initializeVisionModule();
    if(IS_STATUS_NOT_OK(stat))
    {
        LOG_CRITICAL(MAIN_LOG_ID, "Failed to initialize vision module");
        shutdownPlayersModule();
        shutdownGameManager();
        shutdownFrameModule();
        shutdownLoggingModule();
        return -1;
    }

    LOG_INFO(MAIN_LOG_ID, "Finished Initializing");

    while(pollFrames())
    {
        tickGameManager();
    }

    shutdownVisionModule();
    shutdownPlayersModule();
    shutdownGameManager();
    shutdownFrameModule();
    shutdownLoggingModule();
    return 0;
}
