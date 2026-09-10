#pragma once
#include "ball_roll.hpp"
#include "game_lib/components/render_object.hpp"

namespace MiniGolf {
class RenderGolfBall : public RenderObject
{
public:
    Color color{};
    float radius = 16;
    BallOrientation orientation;
    Status render(FrameID frameId) const override;
};
}
