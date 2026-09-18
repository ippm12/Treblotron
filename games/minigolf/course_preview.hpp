#pragma once
#include "minigolf.hpp"
namespace MiniGolf {
// Editor adapter: all simulation and drawing stays in the game's implementation.
// This deliberately bypasses match turns, vision input and player menus.
class CoursePreview {
public:
    explicit CoursePreview(FrameID frame);
    ~CoursePreview();
    void setHole(const CourseHole& hole,bool ball,const std::string& theme="classic");
    void view(Vec2 center,float zoom);
    void advance(float dt);
    void seek(float seconds);
    void draw(bool surfaces=true,bool rails=true);
    void putt(Vec2 velocity);
    void resetBall(Vec2 position);
    bool ready() const;
    bool holed() const;
    Vec2 ballPosition() const;
    float time() const;
private:
    MiniGolfGame game{CourseId::TestHole};
};
}
