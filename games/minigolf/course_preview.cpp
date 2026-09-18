#include "course_preview.hpp"
#include "frame/render_queue.hpp"
#include <cmath>
namespace MiniGolf {
CoursePreview::CoursePreview(FrameID frame) {
    game.m_frameId=frame;
    game.m_world=std::make_unique<PhysicsWorld>();
    game.m_world->setPixelsPerMeter(100);
    game.m_fontId=loadFont("assets/fonts/Roboto-Regular.ttf",28);
    game.m_camera.setViewport(COURSE_VIEW_X,COURSE_VIEW_Y,COURSE_VIEW_W,COURSE_VIEW_H);
}
CoursePreview::~CoursePreview() {game.shutdown();
}
void CoursePreview::setHole(const CourseHole& h,bool ball,const std::string& theme) {
    game.teardownCurrentHole();
    game.m_currentHole=0;
    game.m_currentPlayer=0;
    game.m_course.holes[0]=h;
    game.m_course.theme=theme;
    game.m_players.clear();
    if(ball) {game.m_players.resize(1);
        game.m_players[0].ballColor=BALL_COLORS[0];
    }
    game.buildCurrentHole();
    game.m_phase=Phase::Aiming;
    if(ball) {game.spawnBall(0,h.startPos);
        game.m_players[0].shotStart=h.startPos;
    }
}
void CoursePreview::view(Vec2 center,float zoom) {
    game.m_camera.setWorldBounds(-1000000,-1000000,1000000,1000000);
    game.m_camera.setZoom(zoom);
    game.m_camera.setCenter(center.x,center.y);
}
void CoursePreview::advance(float dt) {game.stepCoursePhysics(dt);
}
void CoursePreview::seek(float seconds) {
    // Editing preview has no ball; moving bodies catch up on the next step.
    if(game.m_players.empty()) game.m_courseTime=std::max(0.0f,seconds);
}
void CoursePreview::draw(bool surfaces,bool rails) {game.renderCourse();
    game.renderObstacles(surfaces,rails);
    game.renderBallTrails();
    game.renderBalls();
    renderQueueSortByLayer(game.m_frameId);
}
bool CoursePreview::ready() const {
    if(game.m_players.empty()) return false;
    const auto& p=game.m_players[0];
    return b2Body_IsValid(p.ballBody) && p.hazardTimer<=0 && getBodySpeedPx(*game.m_world,p.ballBody)<12;
}
bool CoursePreview::holed() const {return !game.m_players.empty() && game.m_players[0].holedOut[0];
}
Vec2 CoursePreview::ballPosition() const {Vec2 p{};
    if(!game.m_players.empty() && b2Body_IsValid(game.m_players[0].ballBody)) getBodyPositionPx(*game.m_world,game.m_players[0].ballBody,p.x,p.y);
    return p;
}
float CoursePreview::time() const {return static_cast<float>(game.m_courseTime);
}
void CoursePreview::putt(Vec2 v) {
    if(!ready()) return;
    auto& p=game.m_players[0];
    p.shotStart=ballPosition();
    p.trail.clear();
    p.respawnProtected=false;
    p.strokes[0]=0;
    p.finishedHole[0]=false;
    float length=std::hypot(v.x,v.y);
    if(length>750) {v.x*=750/length;
        v.y*=750/length;
    }
    setBodyVelocityPx(*game.m_world,p.ballBody,v.x,v.y);
    game.m_phase=Phase::BallInMotion;
}
void CoursePreview::resetBall(Vec2 pos) {
    if(game.m_players.empty()) return;
    auto& p=game.m_players[0];
    if(b2Body_IsValid(p.ballBody)) b2DestroyBody(p.ballBody);
    p=PlayerState{};
    p.ballColor=BALL_COLORS[0];
    p.shotStart=pos;
    game.spawnBall(0,pos);
    game.m_phase=Phase::Aiming;
}
}
