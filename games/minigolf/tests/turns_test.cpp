#include "fixtures/golf_fixture.hpp"
#include <stdexcept>

void test_turns()
{
    GolfFixture fixture;
    [[maybe_unused]] auto& g=fixture.game;
    [[maybe_unused]] auto& course=fixture.course;
    [[maybe_unused]] auto& base=fixture.base;
    {
        // Collecting after two small putts must finish only the original turn.
        g.teardownCurrentHole(); g.m_players.clear(); g.m_players.resize(3);
        g.m_course=course; g.m_currentHole=0; g.buildCurrentHole(); g.beginNextTurn();
        g.processDart({0,0.01f});
        for(int frame=0;frame<180;++frame) g.update(1.0f/60);
        g.processDart({180,0.01f});
        for(int frame=0;frame<180;++frame) g.update(1.0f/60);
        assert(g.m_players[0].strokes[0]==2 && g.m_throwsRemainingInTurn==1);
        g.onTurnSkipped();
        if(g.m_players[0].strokes[0]!=3 || g.m_players[1].strokes[0]!=0 ||
           g.m_players[2].strokes[0]!=0 || g.m_currentPlayer!=1 || g.m_players[2].hasSpawned) {
            std::cerr << "COLLECT REGRESSION: scores " << int(g.m_players[0].strokes[0])
                << "," << int(g.m_players[1].strokes[0]) << "," << int(g.m_players[2].strokes[0])
                << " current=" << int(g.m_currentPlayer) << "\n";
            throw std::runtime_error("collect regression");
        }
    }
    // Exercise the real update/turn path, including bodies removed mid-turn.
    for(int count:{1,2,6}) {
        g.teardownCurrentHole(); g.m_players.resize(count);
        g.m_course=course; g.m_currentHole=0; g.buildCurrentHole();
        unsigned random=12345;
        for(int frame=0;frame<20000 && g.m_phase!=Phase::GameOver;++frame) {
            if(g.m_waitingForCollect) {
                g.m_waitingForCollect=false;
                if(g.m_phase==Phase::HoleTransition) g.advanceToNextHole();
                else g.beginNextTurn();
            }
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
