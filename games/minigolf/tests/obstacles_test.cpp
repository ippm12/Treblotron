#include "fixtures/golf_fixture.hpp"
#include "course_motion.hpp"
#include <stdexcept>

void test_obstacles()
{
    GolfFixture fixture;
    [[maybe_unused]] auto& g=fixture.game;
    [[maybe_unused]] auto& course=fixture.course;
    [[maybe_unused]] auto& base=fixture.base;
    {
        auto h=base;h.walls.clear();h.surfaces.clear();h.lasers={{{705,500},{300,8}}};
        h.lasers[0].angleDegrees=45;
        prepare(g,h);place(g,{775,570});place(g,{805,500},1);
        g.updateObstacles(0.01f);
        assert(g.m_players[0].hazardTimer>0 && g.m_players[1].hazardTimer==0);
        h.lasers.clear();h.surfaces={{{705,500},{300,40},SurfaceKind::Water}};
        h.surfaces[0].angleDegrees=45;
        prepare(g,h);place(g,{775,570});place(g,{805,500},1);g.updateObstacles(0.01f);
        assert(g.m_players[0].waterSplash && g.m_players[1].hazardTimer==0);
        h.surfaces[0].kind=SurfaceKind::Sand;
        assert(surfaceResponse(h,{775,570},0).resistance>5);
        assert(surfaceResponse(h,{805,500},0).resistance==1);
        h.surfaces.clear();h.walls={{705,500,300,20}};h.walls[0].angleDegrees=45;
        prepare(g,h);
        assert(near(b2Rot_GetAngle(b2Body_GetRotation(g.m_wallBodies[4])),0.78539816f));
        // A collision away from the old axis-aligned box deflects sideways.
        place(g,{775,510});setBodyVelocityPx(*g.m_world,g.m_players[0].ballBody,0,400);
        for(int i=0;i<24;++i)g.stepCoursePhysics(1.0f/120);
        float vx,vy;getBodyVelocityPx(*g.m_world,g.m_players[0].ballBody,vx,vy);
        assert(vx>100);
    }
    {
        auto h=base; h.lasers={{{705,500},{250,8},1,2}};
        prepare(g,h); place(g,{705,500}); place(g,{710,500},1);
        const auto origin=g.m_players[0].shotStart;
        g.m_players[0].strokes[0]=1; g.m_throwsRemainingInTurn=2;
        g.updateObstacles(0.01f);
        assert(g.m_players[0].strokes[0]==2);
        assert(g.m_players[1].strokes[0]==1 && g.m_players[1].hazardTimer>0);
        assert(!b2Body_IsEnabled(g.m_players[1].ballBody));
        assert(!b2Body_IsEnabled(g.m_players[0].ballBody));
        for(int i=0;i<100;++i) g.updateObstacles(0.01f);
        assert(g.m_players[0].strokes[0]==2 && g.m_throwsRemainingInTurn==2);
        const auto p=position(g); assert(near(p.x,origin.x) && near(p.y,origin.y));
        assert(b2Body_IsEnabled(g.m_players[0].ballBody));
        assert(g.m_players[1].strokes[0]==1 && g.m_currentPlayer==0);
        g.m_courseTime=1.5; place(g,{705,500}); g.m_players[0].hazardImmunity=0;
        g.updateObstacles(0.01f); assert(g.m_players[0].hazardTimer==0);
    }
    {
        auto h=base; h.surfaces={{{705,500},{180,180},SurfaceKind::Water}};
        prepare(g,h); place(g,{705,500}); g.m_players[0].strokes[0]=STROKE_CAP-1;
        g.updateObstacles(0.01f); assert(g.m_players[0].waterSplash);
        for(int i=0;i<100;++i) g.updateObstacles(0.01f);
        assert(g.m_players[0].strokes[0]==STROKE_CAP && g.m_players[0].finishedHole[0]);
    }
    {
        auto h=base; h.bumpers={{{705,500},40,550}};
        prepare(g,h); place(g,{762,500});
        g.updateObstacles(0.01f);
        float vx=0,vy=0; getBodyVelocityPx(*g.m_world,g.m_players[0].ballBody,vx,vy);
        assert(near(vx,550) && near(vy,0));
        assert(g.m_bumperAnimation[0]>0);
        place(g,{800,600}); g.updateObstacles(0.4f);
        assert(g.m_bumperAnimation[0]==0);
    }
    {
        auto h=base; h.bumpers={{{705,500},40,550}};
        for(const Vec2 incoming: {Vec2{-900,0},Vec2{900,0},Vec2{-200,800},Vec2{2200,0},Vec2{3000,0}}) {
            prepare(g,h); place(g,{762,500});
            setBodyVelocityPx(*g.m_world,g.m_players[0].ballBody,incoming.x,incoming.y);
            g.updateObstacles(0.01f);
            float vx,vy; getBodyVelocityPx(*g.m_world,g.m_players[0].ballBody,vx,vy);
            assert(vx>0);
            assert(near(std::hypot(vx,vy),std::min(2250.0f,std::hypot(incoming.x,incoming.y)+550),0.1f));
            const float kickedSpeed=std::hypot(vx,vy);
            g.updateObstacles(0.01f); // Same contact cannot add another kick.
            getBodyVelocityPx(*g.m_world,g.m_players[0].ballBody,vx,vy);
            assert(near(std::hypot(vx,vy),kickedSpeed,0.1f));
        }
        // Exercise real collision response, not just the proximity handler.
        for(float speed: {900.0f,1600.0f}) {
            prepare(g,h); place(g,{850,500});
            setBodyVelocityPx(*g.m_world,g.m_players[0].ballBody,-speed,0);
            bool kicked=false;
            for(int tick=0;tick<40 && !kicked;++tick) {
                g.stepCoursePhysics(1.0f/120);
                kicked=g.m_bumperAnimation[0]>0;
            }
            float vx,vy; getBodyVelocityPx(*g.m_world,g.m_players[0].ballBody,vx,vy);
            assert(kicked && vx>speed && std::hypot(vx,vy)<=2250.1f);
        }
    }
    {
        auto h=base; h.portals={{{400,500},0,{200,80,200}},{{1000,400},0,{200,80,200}}};
        prepare(g,h); place(g,{412,490});
        setBodyVelocityPx(*g.m_world,g.m_players[0].ballBody,120,-240);
        g.m_players[0].trail.push_back({400,500});
        g.updateObstacles(0.01f);
        const auto p=position(g);
        assert(near(p.x,1012) && near(p.y,390) && g.m_players[0].trail.empty());
        float vx=0,vy=0; getBodyVelocityPx(*g.m_world,g.m_players[0].ballBody,vx,vy);
        assert(near(vx,120) && near(vy,-240));
        // Stay in the destination: cooldown expiry alone cannot ping-pong.
        place(g,{1000,400});
        for(int i=0;i<100;++i) g.updateObstacles(0.01f);
        assert(near(position(g).x,1000));
    }
    {
        auto h=base; h.portals={{{400,500},0,{200,80,200}},{{1000,400},0,{200,80,200}}};
        prepare(g,h); place(g,{400+h.cupRadius+1,500});
        g.updateObstacles(0.01f);
        assert(near(position(g).x,400+h.cupRadius+1)); // Same entry radius.
        h.walls={{1000,400,80,80}};
        prepare(g,h); place(g,{412,490});
        g.updateObstacles(0.01f);
        assert(near(position(g).x,412) && near(position(g).y,490)); // Blocked offset stays at entrance.
    }
    {
        auto h=base;
        h.rails={{{{{0,0},0,100},{{100,0},1,100}},RailMode::PingPong}};
        h.walls={{400,400,100,20,0}};
        h.bumpers={{{900,500},28,550,0}};
        h.cupRail=0;
        prepare(g,h);
        for(int i=0;i<240;++i) {
            constexpr float dt=1.0f/120;
            g.moveObstacles(dt); g.m_world->step(dt); g.m_courseTime+=dt;
            Vec2 actual; getBodyPositionPx(*g.m_world,g.m_wallBodies[4],actual.x,actual.y);
            const auto expected=g.obstaclePosition({400,400},0);
            assert(near(actual.x,expected.x,0.1f));
        }
        const auto cup=g.cupPosition(); place(g,cup);
        g.updateCupInteraction(1.0f/120);
        assert(g.m_players[0].holedOut[0] && !b2Body_IsValid(g.m_players[0].ballBody));
    }
    {
        auto h=base;
        h.rails={{{{{0,0},0,100},{{100,0},1,100}},RailMode::PingPong}};
        h.portals={{{400,500},0,{200,80,200},0},{{1000,400},0,{200,80,200}}};
        prepare(g,h); g.m_courseTime=0.5;
        place(g,{460,490}); // Off-center entrance sweeps over a stationary ball.
        g.updateObstacles(0.01f);
        float vx=0,vy=0; getBodyVelocityPx(*g.m_world,g.m_players[0].ballBody,vx,vy);
        assert(near(vx,-100) && near(vy,0) && near(position(g).x,1010) && near(position(g).y,390));
        for(int i=0;i<60;++i) {
            g.m_world->step(0.01f); g.m_courseTime+=0.01; g.updateObstacles(0.01f);
        }
        assert(position(g).x>800 && position(g).x<960);
        assert(near(railVelocity(h,0,1.5,0.01f).x,0)); // paused portal
        h.portals[1].rail=0;
        prepare(g,h); g.m_courseTime=0.5; place(g,{450,500});
        setBodyVelocityPx(*g.m_world,g.m_players[0].ballBody,20,-30);
        g.updateObstacles(0.01f);
        getBodyVelocityPx(*g.m_world,g.m_players[0].ballBody,vx,vy);
        assert(near(vx,20) && near(vy,-30)); // equal moving frames cancel
    }
    {
        auto h=base; h.lasers={{{705,500},{250,8},1,2}};
        prepare(g,h); place(g,{705,500},1);
        g.m_players[1].strokes[0]=STROKE_CAP;
        g.m_players[1].finishedHole[0]=true;
        g.m_waitingForCollect=true;
        g.updateObstacles(0.01f);
        assert(g.m_players[1].hazardTimer>0 && g.m_players[1].strokes[0]==STROKE_CAP);
        g.m_waitingForCollect=false;
    }
    {
        auto h=base;
        addTileBarrier(h,{400,400},40,{"##","#."},0);
        h.rails={{{{{0,0},0,100},{{100,0},1,100}},RailMode::PingPong}};
        assert(h.walls.size()==3 && h.walls[0].showRail && !h.walls[1].showRail);
        prepare(g,h);
        for(int step=0;step<60;++step) {
            g.moveObstacles(0.01f); g.m_world->step(0.01f); g.m_courseTime+=0.01;
        }
        for(size_t i=0;i<3;++i) {
            Vec2 actual; getBodyPositionPx(*g.m_world,g.m_wallBodies[i+4],actual.x,actual.y);
            assert(near(actual.x,h.walls[i].centerX+60,0.1f));
            assert(near(actual.y,h.walls[i].centerY,0.1f));
        }
        h.surfaces={{{705,500},{100,100},SurfaceKind::Sand}};
        assert(near(surfaceResponse(h,{705,500},0).resistance,
                    surfaceResponse(h,{705,500},10).resistance));
        static_assert(CourseLayer::Guide>CourseLayer::Effect && CourseLayer::Guide+1<GameLayout::SIDEBAR_Z);
    }
    // All nine hole definitions must be buildable and simulate without assertions.
    for(const auto& h:course.holes) {
        prepare(g,h);
        for(const auto& portal:h.portals) {
            int count=0;
            for(const auto& other:h.portals) if(portal.pair==other.pair) {
                ++count;
                assert(portal.color.r==other.color.r && portal.color.g==other.color.g && portal.color.b==other.color.b);
            }
            assert(count==2);
        }
        for(int i=0;i<600;++i) {
            constexpr float dt=1.0f/120;
            g.moveObstacles(dt); g.m_world->step(dt); g.m_courseTime+=dt;
            g.applyRollingFriction(dt); g.updateObstacles(dt); g.updateCupInteraction(dt);
        }
    }
}
