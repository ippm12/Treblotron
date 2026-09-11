#pragma once
#include "course_defs.hpp"
#include <algorithm>
#include <cmath>

namespace MiniGolf {
// Evaluated from absolute simulation time: pauses and corners do not drift
// with frame rate. A loop includes its final segment back to stop zero.
inline Vec2 railOffset(const CourseHole& hole, int index, double time)
{
    if(index < 0 || static_cast<size_t>(index) >= hole.rails.size()) return {};
    const auto& rail = hole.rails[static_cast<size_t>(index)];
    const size_t n = rail.stops.size();
    if(n == 0) return {};
    if(n == 1) return rail.stops[0].offset;
    const size_t legs = rail.mode == RailMode::Loop ? n : 2*(n-1);
    auto node = [n](size_t leg) { return leg < n ? leg : 2*(n-1)-leg; };
    auto endpoints = [&](size_t leg, size_t& a, size_t& b) {
        if(rail.mode == RailMode::Loop) { a=leg; b=(leg+1)%n; }
        else { a=node(leg); b=node(leg+1); }
    };
    auto travel = [&](size_t a, size_t b) {
        const auto p=rail.stops[a].offset, q=rail.stops[b].offset;
        return std::hypot(q.x-p.x,q.y-p.y)/std::max(1.0f,rail.stops[a].speed);
    };
    double duration=0;
    for(size_t leg=0;leg<legs;++leg) {
        size_t a,b; endpoints(leg,a,b);
        duration += std::max(0.0f,rail.stops[a].pauseSeconds)+travel(a,b);
    }
    if(duration <= 0) return rail.stops[0].offset;
    double t=std::fmod(std::max(0.0,time+rail.phaseSeconds),duration);
    for(size_t leg=0;leg<legs;++leg) {
        size_t a,b; endpoints(leg,a,b);
        const float pause=std::max(0.0f,rail.stops[a].pauseSeconds);
        if(t < pause) return rail.stops[a].offset;
        t-=pause;
        const float seconds=travel(a,b);
        if(t < seconds) {
            const float f=static_cast<float>(t)/seconds;
            const auto p=rail.stops[a].offset, q=rail.stops[b].offset;
            return {p.x+(q.x-p.x)*f,p.y+(q.y-p.y)*f};
        }
        t-=seconds;
    }
    return rail.stops[0].offset;
}
inline Vec2 attachedPosition(const CourseHole& hole, Vec2 base, int rail, double time)
{
    const auto o=railOffset(hole,rail,time);
    return {base.x+o.x,base.y+o.y};
}
inline Vec2 railVelocity(const CourseHole& hole,int rail,double time,float dt)
{
    if(dt<=0) return {};
    const auto a=railOffset(hole,rail,time-dt), b=railOffset(hole,rail,time);
    return {(b.x-a.x)/dt,(b.y-a.y)/dt};
}
inline Vec2 portalExitVelocity(Vec2 ball,Vec2 entrance,Vec2 exit)
{
    return {ball.x-entrance.x+exit.x,ball.y-entrance.y+exit.y};
}
inline bool laserActive(const Laser& laser, double time)
{
    const double on=std::max(0.0f,laser.onSeconds), off=std::max(0.0f,laser.offSeconds);
    return on>0 && std::fmod(std::max(0.0,time+laser.phaseSeconds),on+off)<on;
}
inline bool insidePatch(Vec2 p, Vec2 center, Vec2 size, float inset=0)
{
    return std::abs(p.x-center.x)<=std::max(0.0f,size.x*0.5f-inset)
        && std::abs(p.y-center.y)<=std::max(0.0f,size.y*0.5f-inset);
}
struct SurfaceResponse { float resistance=1; float grip=45; };
inline SurfaceResponse surfaceResponse(const CourseHole& h, Vec2 p, double /*time*/)
{
    // Match visual priority, independent of the order patches were authored.
    SurfaceResponse result;
    uint32_t priority=0;
    for(const auto& s:h.surfaces) {
        if(!insidePatch(p,s.center,s.size)) continue;
        if(surfaceLayer(s.kind)<priority) continue;
        priority=surfaceLayer(s.kind);
        switch(s.kind) {
            case SurfaceKind::Sand: result={5.25f,65}; break;
            case SurfaceKind::Rough: result={2.5f,55}; break;
            case SurfaceKind::Ice: result={0.12f,0.5f}; break;
            case SurfaceKind::Water: result={1,45}; break;
        }
    }
    return result;
}
}

