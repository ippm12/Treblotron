#pragma once
// Shared Mini Golf fixtures. Tests are compiled with -fno-access-control.
#include "minigolf.hpp"
#include "frame/render_queue.hpp"
#include "players/players.hpp"
#include "game_lib/game_registry.hpp"
#include "games/main_menu.hpp"
#include "game_lib/components/render_shape.hpp"
#include <SDL3/SDL.h>
#include <cassert>
#include <cmath>
#include <iostream>

using namespace MiniGolf;
inline bool near(float a,float b,float tolerance=0.02f) { return std::abs(a-b)<tolerance; }
inline Vec2 position(MiniGolfGame& g,size_t i=0) {
    Vec2 p; getBodyPositionPx(*g.m_world,g.m_players[i].ballBody,p.x,p.y); return p;
}
inline void place(MiniGolfGame& g,Vec2 p,size_t i=0) {
    b2Body_SetTransform(g.m_players[i].ballBody,g.m_world->pixelsToMeters(p.x,p.y),b2Rot_identity);
}
inline void prepare(MiniGolfGame& g,CourseHole h) {
    g.teardownCurrentHole();
    g.m_options={}; g.m_teams.clear();
    g.m_currentHole=0; g.m_course.holes[0]=h;
    g.m_players.clear(); g.m_players.resize(2);
    g.m_camera.setViewport(COURSE_VIEW_X,COURSE_VIEW_Y,COURSE_VIEW_W,COURSE_VIEW_H);
    g.buildCurrentHole();
    g.beginNextTurn();
    g.spawnBall(1,{h.startPos.x+90,h.startPos.y});
    g.m_players[1].shotStart={h.startPos.x+90,h.startPos.y};
    g.m_phase=Phase::BallInMotion;
}

struct GolfFixture {
    Course course=buildCourse(CourseId::TestHole);
    CourseHole base=course.holes[0];
    MiniGolfGame game{CourseId::TestHole};
    GolfFixture() {
        base.surfaces.clear();
        game.m_world=std::make_unique<PhysicsWorld>();
        game.m_world->setPixelsPerMeter(100);
    }
    ~GolfFixture() { game.teardownCurrentHole(); }
};
