#include "fixtures/golf_fixture.hpp"
#include <stdexcept>

void test_physics()
{
    GolfFixture fixture;
    [[maybe_unused]] auto& g=fixture.game;
    [[maybe_unused]] auto& course=fixture.course;
    [[maybe_unused]] auto& base=fixture.base;
    {
        CourseHole h=base;
        h.rails={{{{{0,0},1,100},{{100,0},2,100}},RailMode::PingPong}};
        assert(near(railOffset(h,0,0.5).x,0));
        assert(near(railOffset(h,0,1.5).x,50));
        assert(near(railOffset(h,0,3).x,100));
        assert(near(railOffset(h,0,4.5).x,50));
        assert(near(railOffset(h,0,5.5).x,0));
        h.rails[0]={{{{0,0},0,100},{{100,0},0,100},{{100,100},0,100}},RailMode::Loop};
        const auto q=railOffset(h,0,2+std::sqrt(2.0)/2);
        assert(near(q.x,50) && near(q.y,50));
        assert(near(railOffset(h,-1,100).x,0));
        assert(!laserActive({{0,0},{100,8},0,0},0));
        Laser l{{0,0},{100,8},1,2};
        assert(laserActive(l,0.9)); assert(!laserActive(l,1)); assert(laserActive(l,3));
    }
    for(auto kind:{SurfaceKind::Rough,SurfaceKind::Sand,SurfaceKind::Ice}) {
        auto h=base; h.surfaces={{{705,500},{200,200},kind}};
        prepare(g,h); place(g,{705,500});
        setBodyVelocityPx(*g.m_world,g.m_players[0].ballBody,500,0);
        g.applyRollingFriction(0.1f);
        const auto response=surfaceResponse(h,{705,500},0);
        assert(near(getBodySpeedPx(*g.m_world,g.m_players[0].ballBody),500-18.5f*response.resistance));
    }
    {
        prepare(g,base); place(g,g.cupPosition(),1);
        g.updateCupInteraction(1.0f/120);
        assert(g.m_players[1].holedOut[0] && g.m_players[1].finishedHole[0]);
        assert(!b2Body_IsValid(g.m_players[1].ballBody) && g.m_currentPlayer==0);
        auto h=base;
        h.surfaces={{{705,500},{200,200},SurfaceKind::Sand},{{705,500},{200,200},SurfaceKind::Ice}};
        assert(near(surfaceResponse(h,{705,500},0).resistance,5.25f));
        std::swap(h.surfaces[0],h.surfaces[1]);
        assert(near(surfaceResponse(h,{705,500},0).resistance,5.25f));
        static_assert(CourseLayer::Water<CourseLayer::Rail && CourseLayer::Rail<CourseLayer::Wall);
        static_assert(CourseLayer::Wall<CourseLayer::Bumper && CourseLayer::Portal<CourseLayer::Cup);
    }
    {
        // Faster centered putts now drop, while speed and alignment still matter.
        for(float speed:{550.0f,650.0f,700.0f,750.0f}) {
            prepare(g,base); place(g,g.cupPosition());
            setBodyVelocityPx(*g.m_world,g.m_players[0].ballBody,speed,0);
            g.updateCupInteraction(1.0f/120);
            assert(g.m_players[0].holedOut[0]==(speed<=700));
        }
        prepare(g,base);
        const auto cup=g.cupPosition();
        place(g,{cup.x,cup.y+base.cupRadius+8});
        setBodyVelocityPx(*g.m_world,g.m_players[0].ballBody,500,0);
        g.updateCupInteraction(1.0f/120);
        assert(!g.m_players[0].holedOut[0]); // Overlap alone is not capture.
        // Centered fast hits continue forward much slower; mirror-image grazing
        // hits deflect symmetrically, with no random kick or energy gain.
        for(float tangent:{-100.0f,0.0f,100.0f}) {
            prepare(g,base); place(g,{cup.x+base.cupRadius*0.75f,cup.y});
            setBodyVelocityPx(*g.m_world,g.m_players[0].ballBody,750,tangent);
            g.updateCupInteraction(1.0f/120);
            float vx,vy; getBodyVelocityPx(*g.m_world,g.m_players[0].ballBody,vx,vy);
            assert(g.m_players[0].cupBounced && !g.m_players[0].holedOut[0]);
            assert(vx>0 && vx<200 && near(vy,tangent*0.55f));
            assert(std::hypot(vx,vy)<200);
            g.updateCupInteraction(1.0f/120);
            float afterX,afterY; getBodyVelocityPx(*g.m_world,g.m_players[0].ballBody,afterX,afterY);
            assert(near(afterX,vx) && near(afterY,vy)); // No repeated strike/suction.
        }
        prepare(g,base); place(g,{cup.x-80,cup.y});
        setBodyVelocityPx(*g.m_world,g.m_players[0].ballBody,900,0);
        for(int step=0;step<60 && !g.m_players[0].cupBounced && !g.m_players[0].holedOut[0];++step)
            g.stepCoursePhysics(1.0f/120);
        float crossingX,crossingY;
        getBodyVelocityPx(*g.m_world,g.m_players[0].ballBody,crossingX,crossingY);
        assert(g.m_players[0].cupBounced && crossingX>0 && crossingX<250 && near(crossingY,0));
        auto moving=base;
        moving.rails={{{{{0,0},0,100},{{200,0},0,100}},RailMode::PingPong}};
        moving.cupRail=0;
        prepare(g,moving); g.m_courseTime=0.5;
        const auto movingCup=g.cupPosition();
        place(g,{movingCup.x+moving.cupRadius*0.75f,movingCup.y});
        setBodyVelocityPx(*g.m_world,g.m_players[0].ballBody,850,100);
        g.updateCupInteraction(1.0f/120);
        getBodyVelocityPx(*g.m_world,g.m_players[0].ballBody,crossingX,crossingY);
        assert(near(crossingX,265,0.1f) && near(crossingY,55,0.1f));
        prepare(g,base); place(g,{cup.x+base.cupRadius*0.75f,cup.y});
        setBodyVelocityPx(*g.m_world,g.m_players[0].ballBody,1250,0);
        g.updateCupInteraction(1.0f/120);
        assert(!g.m_players[0].holedOut[0] && !g.m_players[0].cupBounced);
        assert(near(getBodySpeedPx(*g.m_world,g.m_players[0].ballBody),1250));
    }

}
