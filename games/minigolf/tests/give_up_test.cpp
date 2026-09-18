#include "fixtures/golf_fixture.hpp"
#include "../../../game_lib/game_manager_class.hpp"

void test_give_up()
{
    GolfFixture fixture;
    auto& g=fixture.game;
    prepare(g,fixture.base);
    const auto oldBody=g.m_players[0].ballBody;
    auto& p=g.m_players[0];
    p.strokes[0]=2; p.pendingReturn=true; p.returnDelay=0.3f;
    p.hazardTimer=0.6f; p.destructionTimer=0.2f;
    p.trail.push_back({500,500});
    g.onDartPositionCalculated(0,0.5f);
    assert(g.getPauseActions().size()==1);
    // Exercise the actual pause menu dispatch: Resume, then game-owned action.
    GameManager manager;
    manager.m_currentGame=GamePtr(&g,[](Game*){});
    manager.m_paused=true;
    manager.handlePauseKey(SDLK_DOWN);
    manager.handlePauseKey(SDLK_RETURN);
    assert(!manager.m_paused);
    manager.m_currentGame.reset();
    assert(p.strokes[0]==STROKE_CAP && p.finishedHole[0] && p.holedOut[0]);
    assert(!b2Body_IsValid(oldBody) && !b2Body_IsValid(p.ballBody));
    assert(!p.pendingReturn && p.hazardTimer==0 && p.destructionTimer==0 && p.trail.empty());
    assert(g.m_currentPlayer==1 && g.m_players[1].strokes[0]==0);
    g.update(0.01f);
    assert(g.m_players[1].strokes[0]==0); // No queued dart leaks into the new turn.
    manager.m_currentGame=GamePtr(&g,[](Game*){});
    manager.m_paused=true;
    manager.handlePauseClick(960,540,1); // Four rows: second row is Give up.
    assert(!manager.m_paused);
    manager.m_currentGame.reset();
    assert(g.m_phase==Phase::HoleTransition && g.m_throwsRemainingInTurn==0);
    assert(g.getPauseActions().empty());
    g.onPauseAction(0); // No repeated retirement during transition.
    assert(g.m_players[1].strokes[0]==STROKE_CAP);
    prepare(g,fixture.base);
    g.m_waitingForCollect=true;
    assert(g.getPauseActions().empty());
    g.onPauseAction(0);
    assert(!g.m_players[0].finishedHole[0]);
    g.m_waitingForCollect=false;
}

