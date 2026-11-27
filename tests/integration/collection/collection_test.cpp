#include "cricket.hpp"
#include "dartfleet.hpp"
#include "x01.hpp"
#include "players/players.hpp"
#include <SDL3/SDL.h>
#include <cassert>
#include <iostream>

static void hit(Game& game, int section, float radius)
{
    const float angle = section ? getSectionPositionIndex(section) * 18.0f - 90.0f : 0;
    game.onDartPositionCalculated(angle, radius);
    game.update(0);
}

static void passSecondPlayer(Game& game)
{
    assert(game.awaitingCollection() && game.getBarInfo().playerName == "One");
    game.onDartsCollected();
    assert(game.getBarInfo().playerName == "Two" && game.getBarInfo().throwsRemaining == 3);
    game.onDartsCollected(); // Unreported bounce-outs.
    assert(game.getBarInfo().playerName == "One" && game.getBarInfo().throwsRemaining == 3);
}

static void testCricket()
{
    CricketGame game;
    assert(game.init(INVALID_FRAME_ID) == STATUS_OK);
    for(int section = 20; section >= 15; --section)
    {
        hit(game, section, 0.60f); // Triple closes the target.
        if(game.awaitingCollection()) passSecondPlayer(game);
    }
    hit(game, 0, 0.0f); // Two bull marks.
    assert(!game.awaitingCollection());
    hit(game, 0, 0.06f); // Third mark wins.
    assert(game.awaitingCollection());
    game.onDartsCollected();
    assert(game.getBarInfo().state == GameState::Blank);
    game.shutdown();
}

static void testX01()
{
    X01Game game(X01Variant::V301);
    assert(game.init(INVALID_FRAME_ID) == STATUS_OK);
    for(int i = 0; i < 3; ++i) hit(game, 20, 0.60f);
    passSecondPlayer(game); // 121 remaining.
    hit(game, 20, 0.60f); // 61 remaining.
    // A batch can contain a dart after the bust. It belongs to this visit.
    game.onDartPositionCalculated(-90.0f, 0.60f); // Leaves 1, bust.
    game.onDartPositionCalculated(-90.0f, 0.60f);
    game.update(0);
    assert(game.awaitingCollection());
    game.onDartsCollected();
    game.update(0);
    assert(game.getBarInfo().playerName == "Two" && game.getBarInfo().throwsRemaining == 3);
    game.onDartsCollected();
    hit(game, 20, 0.60f);
    hit(game, 19, 0.60f);
    hit(game, 2, 0.98f); // 121 = T20 + T19 + D2.
    assert(game.awaitingCollection());
    game.onDartsCollected();
    assert(game.getBarInfo().state == GameState::Blank);
    game.shutdown();
}

static void testDartfleet()
{
    DartfleetGame game(false, FleetSize::Small, FleetSize::Small, VolleySize::Three);
    assert(game.init(INVALID_FRAME_ID) == STATUS_OK);
    for(int team = 0; team < 2; ++team)
    {
        game.onDartsCollected(); // A pickup during setup cannot advance a volley.
        game.onKeyDown(SDLK_RETURN);
        for(int ship = 0; ship < 3; ++ship) game.onKeyDown(SDLK_RETURN);
        game.onKeyDown(SDLK_SPACE);
    }
    for(int frame = 0; frame < 180; ++frame) game.update(1.0f / 60); // First-team reveal.
    const auto attacker = game.getBarInfo().playerName;
    assert(!attacker.empty() && game.getBarInfo().throwsRemaining == 3);
    for(int i = 0; i < 3; ++i) hit(game, 20, 1.2f);
    assert(game.awaitingCollection());
    for(int i = 0; i < 120; ++i) game.update(1.0f / 60);
    assert(game.getBarInfo().playerName == attacker);
    game.onDartsCollected();
    assert(game.getBarInfo().playerName != attacker && game.getBarInfo().throwsRemaining == 3);
    game.onDartsCollected();
    assert(game.getBarInfo().playerName == attacker && game.getBarInfo().throwsRemaining == 3);
    game.shutdown();
}

int main()
{
    assert(initializeFrameModule() == STATUS_OK);
    assert(initializePlayersModule() == STATUS_OK);
    for(const char* name : {"One", "Two"}) assert(createPlayer(name) != INVALID_PLAYER_ID);
    testCricket();
    testX01();
    testDartfleet();
    shutdownPlayersModule();
    shutdownFrameModule();
    std::cout << "PASS Cricket closing and win, X01 bust and checkout, Dartfleet volleys\n";
}
