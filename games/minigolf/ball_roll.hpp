#pragma once

#include <cmath>

namespace MiniGolf {
struct RollVector { float x = 0, y = 0, z = 0; };
struct BallOrientation
{
    float w = 1, x = 0, y = 0, z = 0;

    RollVector rotate(RollVector v) const
    {
        const RollVector t{2*(y*v.z-z*v.y), 2*(z*v.x-x*v.z), 2*(x*v.y-y*v.x)};
        return {v.x+w*t.x+y*t.z-z*t.y,
                v.y+w*t.y+z*t.x-x*t.z,
                v.z+w*t.z+x*t.y-y*t.x};
    }

    void advance(RollVector omega, float dt)
    {
        const float speed = std::sqrt(omega.x*omega.x+omega.y*omega.y+omega.z*omega.z);
        if(speed < 0.00001f || dt <= 0) return;
        const float a = speed*dt*0.5f, s = std::sin(a)/speed;
        const float c = std::cos(a), dx = omega.x*s, dy = omega.y*s, dz = omega.z*s;
        const BallOrientation q{c*w-dx*x-dy*y-dz*z,
            c*x+dx*w+dy*z-dz*y, c*y-dx*z+dy*w+dz*x, c*z+dx*y-dy*x+dz*w};
        const float inv = 1/std::sqrt(q.w*q.w+q.x*q.x+q.y*q.y+q.z*q.z);
        w=q.w*inv; x=q.x*inv; y=q.y*inv; z=q.z*inv;
    }
};

struct BallRoll
{
    BallOrientation orientation;
    RollVector angularVelocity;

    // Screen X/Y, with positive Z facing the viewer. Grip is a visual
    // spin response rate (1/s), independent of translational resistance.
    void update(float vx, float vy, float radius, float dt, float grip = 45.0f)
    {
        if(dt <= 0 || radius <= 0) return;
        const RollVector target{-vy/radius, vx/radius, 0};
        const float decay = std::exp(-grip*dt);
        const float weight = grip > 0 ? (1-decay)/(grip*dt) : 1.0f;
        const RollVector average{target.x+(angularVelocity.x-target.x)*weight,
            target.y+(angularVelocity.y-target.y)*weight,
            target.z+(angularVelocity.z-target.z)*weight};
        orientation.advance(average, dt);
        angularVelocity = {target.x+(angularVelocity.x-target.x)*decay,
            target.y+(angularVelocity.y-target.y)*decay,
            target.z+(angularVelocity.z-target.z)*decay};
    }
};
}
