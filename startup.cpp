/**
 * startup.cpp
 *
 * The main function that runbs on startup
 */

#include <chrono>
#include <memory>
#include <thread>
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

    // Some vision sources (TensorRT on Jetson) build their engine on a
    // background thread during init(). Keep the window alive with a
    // loading screen until the source reports ready, then fall through
    // to the normal game loop.
    //
    // If the build fails (e.g. the ONNX is missing required tensor
    // names), we stay on the loading screen — which will render the
    // error message — until the user closes the window. We never fall
    // through to the game loop with a broken vision system.
    {
        auto lastTick = std::chrono::steady_clock::now();
        bool quitRequested = false;

        // A remote server is not something to wait for. It may be off, or its
        // address may not be set yet, and neither should hold the game hostage
        // at the door — the link indicator and the settings overlay handle it
        // from inside the running app. Local model builds still wait, because
        // there is nothing to show until they finish.
#ifdef DARTLENS_USE_NETWORK
        constexpr bool blockOnVisionInit = false;
#else
        constexpr bool blockOnVisionInit = true;
#endif

        while(blockOnVisionInit && (isVisionInitializing() || isVisionFailed()))
        {
            if(!pollFrames())
            {
                quitRequested = true;
                break;
            }
            const auto now = std::chrono::steady_clock::now();
            const float dt = std::chrono::duration<float>(now - lastTick).count();
            lastTick = now;
            presentVisionLoadingFrame(dt);
            // 60 Hz cap — the loading screen doesn't need anything faster,
            // and we don't want to starve the build thread for CPU.
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }

        if(quitRequested)
        {
            shutdownVisionModule();
            shutdownPlayersModule();
            shutdownGameManager();
            shutdownFrameModule();
            shutdownLoggingModule();
            return 0;
        }
    }

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
