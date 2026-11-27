#include "fixtures/golf_fixture.hpp"
#include <stdexcept>

void test_turns()
{
    GolfFixture fixture;
    [[maybe_unused]] auto& g=fixture.game;
    [[maybe_unused]] auto& course=fixture.course;
    [[maybe_unused]] auto& base=fixture.base;
    {
        // An input received during motion must not become the next player's
        // first shot, including the frame that settles the previous ball.
        prepare(g,base);
        g.m_throwsRemainingInTurn=0;
        g.m_settleTimer=1.0f;
        g.onDartPositionCalculated(0,0.2f);
        g.update(0);
        assert(g.m_currentPlayer == 0 && g.m_waitingForCollect);
        for(int frame=0; frame<120; ++frame) g.update(1.0f/60);
        assert(g.m_currentPlayer == 0);
        g.onDartsCollected();
        assert(g.m_currentPlayer==1 && g.m_phase==Phase::Aiming);
        g.update(0);
        assert(g.m_players[1].strokes[0]==0);
        // A fresh input after the handoff still works.
        g.onDartPositionCalculated(0,0.2f);
        g.update(0);
        assert(g.m_players[1].strokes[0]==1);
    }
    for(int blocked=0;blocked<3;++blocked) {
        prepare(g,base);
        g.m_phase=Phase::Aiming;
        auto& p=g.m_players[0];
        if(blocked==0) p.hazardTimer=1;
        if(blocked==1) p.pendingReturn=true;
        if(blocked==2) setBodyVelocityPx(*g.m_world,p.ballBody,100,0);
        g.onDartPositionCalculated(0,0.2f);
        g.update(0);
        DartPosition queued;
        assert(!g.popDartPosition(queued));
        assert(p.strokes[0]==0);
    }
    {
        // Collecting after two small putts must finish only the original turn.
        g.teardownCurrentHole(); g.m_players.clear(); g.m_players.resize(3);
        g.m_course=course; g.m_currentHole=0; g.buildCurrentHole(); g.beginNextTurn();
        g.processDart({0,0.01f});
        for(int frame=0;frame<180;++frame) g.update(1.0f/60);
        g.processDart({180,0.01f});
        for(int frame=0;frame<180;++frame) g.update(1.0f/60);
        assert(g.m_players[0].strokes[0]==2 && g.m_throwsRemainingInTurn==1);
        g.onDartsCollected();
        g.update(0);
        if(g.m_players[0].strokes[0]!=3 || g.m_players[1].strokes[0]!=0 ||
           g.m_players[2].strokes[0]!=0 || g.m_currentPlayer!=1 || g.m_players[2].hasSpawned) {
            std::cerr << "COLLECT REGRESSION: scores " << int(g.m_players[0].strokes[0])
                << "," << int(g.m_players[1].strokes[0]) << "," << int(g.m_players[2].strokes[0])
                << " current=" << int(g.m_currentPlayer) << "\n";
            throw std::runtime_error("collect regression");
        }
        // Player 2 has three bounce-outs. Collecting the empty board must
        // hand off to player 3 with their complete throw budget.
        g.onDartsCollected();
        g.update(0);
        assert(g.m_currentPlayer == 2 && g.m_players[1].strokes[0] == 3);
        assert(g.m_players[2].strokes[0] == 0 && g.m_throwsRemainingInTurn == 3);
    }
    {
        // The hand gesture arrives while the first putt is still rolling.
        prepare(g,base);
        g.m_phase = Phase::Aiming;
        g.processDart({0,0.01f});
        g.onDartsCollected();
        assert(g.m_currentPlayer == 0 && g.m_phase == Phase::BallInMotion);
        for(int frame=0; frame<240; ++frame) g.update(1.0f/60);
        assert(g.m_players[0].strokes[0] == 3);
        assert(g.m_currentPlayer == 1 && g.m_throwsRemainingInTurn == 3);
        assert(g.m_players[1].strokes[0] == 0);
    }
    // Exercise the real update/turn path, including bodies removed mid-turn.
    for(int count:{1,2,6}) {
        g.teardownCurrentHole(); g.m_players.resize(count);
        g.m_course=course; g.m_currentHole=0; g.buildCurrentHole();
        unsigned random=12345;
        for(int frame=0;frame<20000 && g.m_phase!=Phase::GameOver;++frame) {
            if(g.m_waitingForCollect) g.onDartsCollected();
            if(g.m_phase==Phase::Aiming && frame%30==0) {
                random=random*1664525u+1013904223u;
                const float x=static_cast<float>(random%2001)/1000-1;
                random=random*1664525u+1013904223u;
                const float y=static_cast<float>(random%2001)/1000-1;
                g.processDart({x,y});
            }
            g.update(1.0f/60);
        }
    }
}
