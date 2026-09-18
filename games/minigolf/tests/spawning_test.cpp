#include "fixtures/golf_fixture.hpp"
#include <stdexcept>

void test_spawning()
{
    GolfFixture fixture;
    [[maybe_unused]] auto& g=fixture.game;
    [[maybe_unused]] auto& course=fixture.course;
    [[maybe_unused]] auto& base=fixture.base;
    {
        // A moving hazard can cover the return point. One entry still costs
        // only one penalty until the player explicitly takes another shot.
        auto h=base; h.surfaces={{{705,500},{180,180},SurfaceKind::Water}};
        prepare(g,h); place(g,{705,500}); g.m_players[0].shotStart={705,500};
        g.updateObstacles(0.01f);
        for(int i=0;i<400;++i) g.updateObstacles(0.01f);
        assert(g.m_players[0].strokes[0]==1 && g.m_players[0].hazardTimer==0);
        g.processDart({0,0});
        g.updateObstacles(0.01f);
        assert(g.m_players[0].strokes[0]==3); // previous penalty, new shot, new penalty
    }
    {
        prepare(g,base); g.teardownCurrentHole(); g.buildCurrentHole();
        assert(!b2Body_IsValid(g.m_players[0].ballBody) && !b2Body_IsValid(g.m_players[1].ballBody));
        g.beginNextTurn();
        assert(near(position(g).x,base.startPos.x) && !g.m_players[1].hasSpawned);
        g.m_players[0].shotStart={500,700};
        g.m_currentPlayer=1; g.beginNextTurn();
        assert(!b2Body_IsValid(g.m_players[0].ballBody));
        assert(near(position(g,1).x,base.startPos.x));
        g.returnDisplacedBalls(0.41f);
        assert(near(position(g).x,500) && g.m_players[0].strokes[0]==0);
        // A reset onto an occupied return point removes that occupant too.
        g.m_players[1].shotStart={500,700};
        g.spawnBall(1,g.m_players[1].shotStart);
        g.returnDisplacedBalls(0.41f);
        assert(!g.m_players[0].pendingReturn && !g.ballPositionOccupied(position(g),0));
        // Optional tees are exact, never spread or jittered.
        auto h=base; h.spawnPositions={{300,700},{900,700}};
        g.teardownCurrentHole(); g.m_course.holes[0]=h; g.buildCurrentHole();
        g.beginNextTurn(); assert(near(position(g).x,300));
        g.m_currentPlayer=1; g.beginNextTurn(); assert(near(position(g,1).x,900));
    }
    {
        prepare(g,base);
        g.m_players[0].shotStart={500,700};
        g.m_players[1].shotStart={900,700};
        place(g,{500,700},1);
        g.penalizeHazard(0,{700,500},true);
        for(int step=0;step<150;++step) g.updateObstacles(0.01f);
        assert(near(position(g).x,500) && near(position(g,1).x,900));
        assert(g.m_players[0].strokes[0]==1 && g.m_players[1].strokes[0]==0);
    }
    {
        prepare(g,base);
        g.m_cascadeParticipants=0;
        g.m_players[0].shotStart={500,700};
        g.m_players[1].shotStart={500,700};
        place(g,{500,700},1);
        g.spawnBall(0,{500,700});
        assert(g.m_players[1].pendingReturn && g.m_players[1].destructionTimer>0);
        g.returnDisplacedBalls(0.2f);
        assert(!b2Body_IsValid(g.m_players[1].ballBody));
        g.m_currentPlayer=1; g.beginNextTurn();
        assert(!b2Body_IsValid(g.m_players[1].ballBody)); // turns cannot skip the delay
        g.returnDisplacedBalls(0.21f);
        assert(b2Body_IsValid(g.m_players[1].ballBody));
        assert(!g.ballPositionOccupied(position(g,1),1));
        assert(std::hypot(position(g,1).x-500,position(g,1).y-700)>=36);
        assert(g.m_players[0].strokes[0]==0 && g.m_players[1].strokes[0]==0);
    }
    {
        // A -> B -> C -> A: the root survives, the final return relocates.
        g.teardownCurrentHole(); g.m_players.resize(3); g.m_course.holes[0]=base; g.buildCurrentHole();
        g.spawnBall(0,{400,700}); g.spawnBall(1,{600,700}); g.spawnBall(2,{800,700});
        g.m_cascadeParticipants=0;
        g.m_players[1].shotStart={800,700}; g.m_players[2].shotStart={600,700};
        g.spawnBall(0,{600,700});
        g.returnDisplacedBalls(0.41f);
        assert(g.m_players[2].pendingReturn && near(g.m_players[2].returnDelay,0.4f));
        g.returnDisplacedBalls(0.41f);
        for(size_t i=0;i<3;++i) assert(b2Body_IsValid(g.m_players[i].ballBody) && !g.ballPositionOccupied(position(g,i),i));
        assert(near(position(g).x,600) && g.m_cascadeParticipants==0);
    }
    {
        prepare(g,base); g.m_cascadeParticipants=0;
        g.m_players[1].shotStart=position(g);
        g.spawnBall(1,position(g)); // remove zero; force a loop at the tee
        g.m_players[0].safeReturnRequired=true;
        g.m_course.holes[0].surfaces={{{705,505},{2000,2000},SurfaceKind::Water}};
        g.returnDisplacedBalls(0.41f);
        assert(g.m_players[0].pendingReturn && !b2Body_IsValid(g.m_players[0].ballBody));
        g.m_course.holes[0].surfaces.clear();
        g.returnDisplacedBalls(0.41f);
        assert(!g.m_players[0].pendingReturn && !g.ballPositionOccupied(position(g),0));
    }
}
