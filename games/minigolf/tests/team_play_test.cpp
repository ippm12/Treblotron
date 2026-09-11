#include "fixtures/golf_fixture.hpp"
#include <stdexcept>

void test_team_play()
{
    GolfFixture fixture;
    [[maybe_unused]] auto& g=fixture.game;
    [[maybe_unused]] auto& course=fixture.course;
    [[maybe_unused]] auto& base=fixture.base;
    assert(initializePlayersModule()==STATUS_OK);
    const auto teamA=createTeam("Team A"),teamB=createTeam("Team B");
    for(int i=0;i<4;++i) {
        const auto id=createPlayer("Member "+std::to_string(i+1));
        assert(setPlayerTeam(id,i<3 ? teamA:teamB)==STATUS_OK);
    }
    auto setupTeams=[&](TeamMode mode) {
        g.teardownCurrentHole(); g.m_options={}; g.m_options.teams=mode; g.m_options.ballCollisions=false;
        g.configureTeams(4); g.m_players.clear(); g.m_players.resize(g.m_teams.size());
        g.m_course=course; g.m_currentHole=0; g.buildCurrentHole(); g.beginNextTurn();
    };
    {
        setupTeams(TeamMode::AlternateShot);
        assert(g.m_players.size()==2 && g.activeMember()==0);
        const auto shared=g.m_players[0].ballBody;
        g.onTurnSkipped(); assert(g.m_currentPlayer==1 && g.activeMember()==3);
        g.onTurnSkipped(); assert(g.m_currentPlayer==0 && g.activeMember()==1);
        assert(g.m_players[0].ballBody.index1==shared.index1);
        g.onTurnSkipped(); g.onTurnSkipped();
        assert(g.m_currentPlayer==0 && g.activeMember()==2 && g.m_players[0].strokes[0]==6);
        g.advanceToNextHole(); g.beginNextTurn();
        assert(g.m_teams[0].cursor==1 && g.m_currentPlayer==1);
    }
    {
        setupTeams(TeamMode::Scramble);
        for(int attempt=0;attempt<3;++attempt) {
            assert(g.activeMember()==attempt);
            assert(near(position(g).x,course.holes[0].startPos.x));
            place(g,{500.0f+100*attempt,700});
            g.onTurnSkipped();
        }
        assert(g.m_phase==Phase::ScrambleChoice && g.m_players[0].strokes[0]==0);
        assert(g.m_teams[0].attempts.size()==3);
        g.chooseScramble(1);
        assert(g.m_players[0].strokes[0]==3 && near(position(g).x,600));
        assert(g.m_currentPlayer==1 && g.activeMember()==3);
        g.onTurnSkipped(); assert(g.m_phase==Phase::ScrambleChoice);
        g.chooseScramble(0);
        assert(g.m_currentPlayer==0 && g.m_players[0].strokes[0]==3);
        assert(g.m_teams[0].baseStrokes==3);
        // Selecting a one-putt hole-out commits only that attempt's score.
        g.m_players[0].strokes[0]=4;
        g.holeOutPlayer(0); g.onBallSettled();
        g.onTurnSkipped(); g.onTurnSkipped();
        assert(g.m_phase==Phase::ScrambleChoice);
        assert(g.m_teams[0].attempts[0].holed);
        g.onMouseClick(COURSE_VIEW_W+50,195,1);
        assert(g.m_players[0].strokes[0]==4 && g.m_players[0].holedOut[0]);
        assert(!b2Body_IsValid(g.m_players[0].ballBody));
    }
    g.teardownCurrentHole(); g.m_options={}; g.m_teams.clear();
    shutdownPlayersModule();
}
