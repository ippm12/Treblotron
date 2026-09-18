#include "fixtures/golf_fixture.hpp"

void test_crushing()
{
    GolfFixture fixture;
    auto& g=fixture.game;
    auto press=fixture.base;
    press.walls={{600,500,40,160,0},{680,500,40,160}};
    press.rails={{{{{0,0},0,40},{{24,0},2,40}},RailMode::PingPong}};
    auto step=[&](int ticks){for(int tick=0;tick<ticks;++tick)g.stepCoursePhysics(1.0f/120);};
    for(size_t player:{size_t{0},size_t{1}}) {
        prepare(g,press); place(g,{640,500},player);
        auto& ball=g.m_players[player];
        ball.strokes[0]=2;
        const auto origin=ball.shotStart;
        step(24); // Initial gap was 40 px: just reaching the 32 px ball diameter.
        assert(ball.hazardTimer==0 && ball.strokes[0]==2);
        for(int tick=0;tick<100 && ball.hazardTimer==0;++tick) step(1);
        assert(ball.hazardTimer>0 && !ball.waterSplash && ball.strokes[0]==3);
        assert(!b2Body_IsEnabled(ball.ballBody));
        assert(g.m_players[1-player].strokes[0]==0);
        step(110);
        assert(ball.hazardTimer==0 && ball.strokes[0]==3 && b2Body_IsEnabled(ball.ballBody));
        const auto restored=position(g,player);
        assert(near(restored.x,origin.x) && near(restored.y,origin.y));
    }
    // A stationary narrow gap and ordinary bouncing do not create penalties.
    auto narrow=press; narrow.walls[0].rail=-1; narrow.walls[1].centerX=669;
    prepare(g,narrow); place(g,{635,500}); step(180);
    assert(g.m_players[0].strokes[0]==0 && g.m_players[0].hazardTimer==0);
    auto passage=press; passage.walls[0].rail=-1; passage.walls[1].centerX=720;
    prepare(g,passage); place(g,{660,500});
    setBodyVelocityPx(*g.m_world,g.m_players[0].ballBody,900,0); step(180);
    assert(g.m_players[0].strokes[0]==0);
    // Adjacent faces in a corner are not opposing crusher faces.
    auto corner=fixture.base;
    corner.walls={{600,500,40,160},{660,580,160,40}};
    prepare(g,corner); place(g,{635,545}); step(120);
    assert(g.m_players[0].strokes[0]==0);
    // A press that reverses before the persistence window must not charge.
    auto brief=press; brief.rails[0].stops[1]={{10,0},0,40};
    prepare(g,brief); place(g,{640,500}); step(100);
    assert(g.m_players[0].strokes[0]==0 && g.m_players[0].hazardTimer==0);
    // A rail may stop while still squeezing; paused pressure still counts.
    auto paused=press; paused.rails[0].stops[1]={{12,0},2,80};
    prepare(g,paused); place(g,{640,500}); step(60);
    assert(g.m_players[0].hazardTimer>0 && g.m_players[0].strokes[0]==1);
    // Boundaries count as walls too.
    auto boundary=fixture.base;
    boundary.walls={{boundary.areaTopLeft.x+60,500,40,160,0}};
    boundary.rails={{{{{0,0},0,40},{{-24,0},2,40}},RailMode::PingPong}};
    prepare(g,boundary); place(g,{boundary.areaTopLeft.x+20,500}); step(60);
    assert(g.m_players[0].hazardTimer>0 && g.m_players[0].strokes[0]==1);
    // Translating together is not a closing press, even in a tight passage.
    auto conveyor=narrow; conveyor.walls[0].rail=0; conveyor.walls[1].rail=0;
    prepare(g,conveyor); place(g,{635,500}); step(120);
    assert(g.m_players[0].strokes[0]==0);
    // A bad shot origin still receives the usual protection against repeated
    // hazard penalties while the returning ball remains in the press.
    prepare(g,press); place(g,{640,500});
    g.m_players[0].shotStart={640,500}; step(220);
    assert(g.m_players[0].strokes[0]==1);
    prepare(g,press); place(g,{640,500}); g.m_players[0].strokes[0]=STROKE_CAP-1;
    step(180);
    assert(g.m_players[0].strokes[0]==STROKE_CAP && g.m_players[0].finishedHole[0]);

}
