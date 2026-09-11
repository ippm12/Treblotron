#include "fixtures/golf_fixture.hpp"
#include <stdexcept>

void test_settings()
{
    GolfFixture fixture;
    [[maybe_unused]] auto& g=fixture.game;
    [[maybe_unused]] auto& course=fixture.course;
    [[maybe_unused]] auto& base=fixture.base;
    {
        auto descriptor=*std::find_if(getRegisteredGames().begin(),getRegisteredGames().end(),
            [](const auto& d){ return d.name=="Mini Golf"; });
        std::vector<size_t> choices;
        descriptor.updateSettings(descriptor.settings,choices);
        assert(choices[2]==2 && descriptor.settings[3].options.size()==1);
        for(size_t n=0;n<3;++n) {
            choices[2]=n; descriptor.updateSettings(descriptor.settings,choices);
            assert(descriptor.settings[3].options.size()==(n==2 ? 1u:3u));
            for(size_t start=0;start<(n==2 ? 1u:3u);++start) {
                GameOptions options; options.holeCount=static_cast<uint8_t>((n+1)*3);
                options.startHole=static_cast<uint8_t>(start*3);
                const auto selected=selectedCourse(CourseId::TestHole,options);
                for(size_t hole=0;hole<options.holeCount;++hole)
                    assert(std::string(selected.holes[hole].name)==course.holes[(start*3+hole)%9].name);
                prepare(g,base); g.m_options=options; g.m_course=selected;
                g.teardownCurrentHole(); g.buildCurrentHole();
                for(size_t hole=0;hole<options.holeCount;++hole) g.advanceToNextHole();
                assert(g.m_phase==Phase::GameOver);
                g.update(1.0f/60); // Finished shortened round must not step destroyed walls.
            }
        }
        choices[2]=2; choices[3]=2; descriptor.updateSettings(descriptor.settings,choices);
        assert(choices[3]==0);
    }
    {
        prepare(g,base); g.m_options.ballCollisions=false;
        g.spawnBall(0,{600,700}); g.spawnBall(1,{600,700});
        for(int i=0;i<30;++i) g.m_world->step(1.0f/120);
        assert(near(position(g).x,600) && near(position(g,1).x,600));
        assert(!g.m_players[0].pendingReturn && !g.m_players[1].pendingReturn);
        g.m_players[0].shotStart={600,700};
        g.penalizeHazard(0,{600,700},true);
        for(int i=0;i<100;++i) g.updateObstacles(0.01f);
        assert(b2Body_IsValid(g.m_players[1].ballBody) && !g.m_players[1].pendingReturn);
        place(g,{base.areaTopLeft.x+BALL_RADIUS_PX+1,700});
        setBodyVelocityPx(*g.m_world,g.m_players[0].ballBody,-300,0);
        for(int i=0;i<30;++i) g.m_world->step(1.0f/120);
        float vx,vy; getBodyVelocityPx(*g.m_world,g.m_players[0].ballBody,vx,vy);
        assert(vx>0); // Disabling ball contacts still permits wall rebounds.
    }
}
