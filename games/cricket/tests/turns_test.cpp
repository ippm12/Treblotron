#include "cricket.hpp"
#include "players/players.hpp"
#ifdef TREBLOTRON_USE_SIM
#include "game_lib/game_manager.hpp"
#include "vision/vision.hpp"
#include <SDL3/SDL.h>
#include <string_view>
#endif
#include <cassert>
#include <iostream>

#ifdef TREBLOTRON_USE_SIM
static void testSimulatorHandoff()
{
    assert(initializeGameManager() == STATUS_OK);
    assert(initializeVisionModule() == STATUS_OK);
    auto game = std::make_shared<CricketGame>();
    assert(loadGame(game) == STATUS_OK);
    int count = 0;
    SDL_Window** windows = SDL_GetWindows(&count);
    SDL_WindowID simulator = 0;
    SDL_WindowID scoreboard = 0;
    for(int i = 0; i < count; ++i)
    {
        const std::string_view title(SDL_GetWindowTitle(windows[i]));
        if(title == "Treblotron Simulator") simulator = SDL_GetWindowID(windows[i]);
        if(title == "Treblotron") scoreboard = SDL_GetWindowID(windows[i]);
    }
    SDL_free(windows);
    assert(simulator && scoreboard);
    auto click = [&](float x, float y) {
        SDL_Event event{};
        event.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
        event.button.windowID = simulator;
        event.button.button = SDL_BUTTON_LEFT;
        event.button.x = x;
        event.button.y = y;
        assert(SDL_PushEvent(&event));
    };
    // Queue the final throw, pickup, and next player's first throw before
    // the game can tick. Each must reach the right player through vision.
    game->onMissedThrow();
    game->onMissedThrow();
    click(300, 280);
    click(300, 620);
    click(300, 280);
    assert(pollFrames());
    SDL_Delay(70); // Simulator position delay is 50 ms.
    tickGameManager();
    assert(game->getBarInfo().playerName == "One" && game->awaitingCollection());
    tickGameManager();
    assert(game->getBarInfo().playerName == "Two" && game->getBarInfo().throwsRemaining == 3);
    SDL_Delay(70);
    tickGameManager();
    assert(game->getBarInfo().playerName == "Two" && game->getBarInfo().throwsRemaining == 2);
    click(300, 620);
    assert(pollFrames());
    tickGameManager();
    assert(game->getBarInfo().playerName == "Three");
    // No dart detected for Three. One empty-board gesture returns to One.
    click(300, 620);
    assert(pollFrames());
    tickGameManager();
    tickGameManager();
    assert(game->getBarInfo().playerName == "One" && game->getBarInfo().throwsRemaining == 3);
    // A pickup during pause can complete a turn already waiting for collection.
    for(int i = 0; i < 3; ++i) game->onMissedThrow();
    auto pressEscape = [&] {
        SDL_Event event{};
        event.type = SDL_EVENT_KEY_DOWN;
        event.key.windowID = scoreboard;
        event.key.key = SDLK_ESCAPE;
        assert(SDL_PushEvent(&event));
        assert(pollFrames());
    };
    pressEscape();
    click(300, 620);
    assert(pollFrames());
    tickGameManager();
    assert(game->getBarInfo().playerName == "One");
    pressEscape();
    tickGameManager();
    assert(game->getBarInfo().playerName == "Two" && game->getBarInfo().throwsRemaining == 3);
    shutdownGameManager();
    shutdownVisionModule();
}
#endif

int main()
{
    assert(initializeFrameModule() == STATUS_OK);
    assert(initializePlayersModule() == STATUS_OK);
    for(const char* name : {"One", "Two", "Three"}) assert(createPlayer(name) != INVALID_PLAYER_ID);
    CricketGame game;
    assert(game.init(INVALID_FRAME_ID) == STATUS_OK);

    // Full first turn. Wait for the actual collection before rotating.
    for(int i = 0; i < 3; ++i)
    {
        game.onDartPositionCalculated(0, 1.2f);
        game.update(0);
    }
    assert(game.getBarInfo().state == GameState::CollectDarts);
    for(int frame=0; frame<120; ++frame) game.update(1.0f/60);
    assert(game.getBarInfo().playerName == "One");
    game.onDartsCollected();
    game.update(0);
    assert(game.getBarInfo().playerName == "Two" && game.getBarInfo().throwsRemaining == 3);

    // No detected dart in the second turn. Three manually reported misses
    // still wait for the empty-board gesture before starting the third turn.
    for(int i=0; i<3; ++i) game.onMissedThrow();
    for(int frame=0; frame<120; ++frame) game.update(1.0f/60);
    assert(game.getBarInfo().playerName == "Two");
    game.onDartsCollected();
    game.update(0);
    assert(game.getBarInfo().playerName == "Three" && game.getBarInfo().throwsRemaining == 3);
    // Unreported bounce-outs also end with a single gesture.
    game.onDartsCollected();
    game.update(0);
    assert(game.getBarInfo().playerName == "One" && game.getBarInfo().throwsRemaining == 3);
    game.shutdown();
#ifdef TREBLOTRON_USE_SIM
    testSimulatorHandoff();
#endif
    shutdownPlayersModule();
    shutdownFrameModule();
    std::cout << "PASS cricket/turns\n";
}
