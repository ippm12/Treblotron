#include "game_lib/game.hpp"
#include <cassert>
#include <iostream>

class TurnGame : public Game {
public:
    TurnGame() : Game("turn skip test") {}
    int remaining=3, calls=0, turn=0;
    bool progress=true, changeName=true, playable=true;
    Status init(FrameID) override { return STATUS_OK; }
    void update(float) override {}
    void render() override {}
    void shutdown() override {}
    GameBarInfo getBarInfo() const override {
        GameBarInfo info;
        info.state=playable ? GameState::PlayerTurn : GameState::Blank;
        info.playerName=changeName ? std::to_string(turn) : "same player";
        info.throwsRemaining=static_cast<uint8_t>(remaining);
        return info;
    }
    void onMissedThrow() override {
        ++calls;
        if(progress && --remaining==0) { ++turn; remaining=3; }
    }
};
int main() {
    TurnGame one; one.remaining=1; one.onTurnSkipped();
    assert(one.calls==1 && one.turn==1 && one.remaining==3);
    TurnGame full; full.onTurnSkipped();
    assert(full.calls==3 && full.turn==1);
    TurnGame solo; solo.changeName=false; solo.onTurnSkipped();
    assert(solo.calls==3 && solo.turn==1);
    TurnGame stalled; stalled.progress=false; stalled.onTurnSkipped();
    assert(stalled.calls==1);
    TurnGame blank; blank.playable=false; blank.onTurnSkipped(); assert(blank.calls==0);
    TurnGame exhausted; exhausted.remaining=0; exhausted.onTurnSkipped(); assert(exhausted.calls==0);
    std::cout << "PASS game_lib/turn_skipping" << std::endl;
}
