#include "minigolf.hpp"
#include "game_lib/components/render_shape.hpp"
#include "game_lib/components/render_text.hpp"
#include "frame/render_queue.hpp"
#include <algorithm>
#include <cmath>

namespace MiniGolf {
Vec2 MiniGolfGame::obstaclePosition(Vec2 base, int rail) const
{
    return attachedPosition(m_course.holes[m_currentHole],base,rail,m_courseTime);
}
Vec2 MiniGolfGame::cupPosition() const
{
    const auto& h=m_course.holes[m_currentHole];
    return obstaclePosition(h.cupPos,h.cupRail);
}
void MiniGolfGame::buildObstacles()
{
    const auto& h=m_course.holes[m_currentHole];
    for(size_t i=0;i<h.walls.size();++i) {
        const auto& w=h.walls[i];
        auto body=m_wallBodies[i+4];
        const auto p=obstaclePosition({w.centerX,w.centerY},w.rail);
        b2Body_SetTransform(body,m_world->pixelsToMeters(p.x,p.y),b2Rot_identity);
        if(w.rail>=0) b2Body_SetType(body,b2_kinematicBody);
    }
    m_bumperAnimation.assign(h.bumpers.size(),0);
    for(const auto& b:h.bumpers) {
        const auto p=obstaclePosition(b.center,b.rail);
        b2BodyDef def=b2DefaultBodyDef();
        def.type=b.rail>=0 ? b2_kinematicBody : b2_staticBody;
        def.position=m_world->pixelsToMeters(p.x,p.y);
        auto body=b2CreateBody(m_world->id(),&def);
        b2ShapeDef shape=b2DefaultShapeDef();
        shape.material.restitution=0.9f;
        b2Circle circle{{0,0},m_world->pixelsToMeters(b.radius)};
        b2CreateCircleShape(body,&shape,&circle);
        m_bumperBodies.push_back(body);
    }
}
void MiniGolfGame::moveObstacles(float dt)
{
    const auto& h=m_course.holes[m_currentHole];
    auto move=[&](b2BodyId body,Vec2 base,int rail) {
        if(rail<0) return;
        const auto target=attachedPosition(h,base,rail,m_courseTime+dt);
        float x=0,y=0; getBodyPositionPx(*m_world,body,x,y);
        setBodyVelocityPx(*m_world,body,(target.x-x)/dt,(target.y-y)/dt);
    };
    for(size_t i=0;i<h.walls.size();++i)
        move(m_wallBodies[i+4],{h.walls[i].centerX,h.walls[i].centerY},h.walls[i].rail);
    for(size_t i=0;i<h.bumpers.size();++i)
        move(m_bumperBodies[i],h.bumpers[i].center,h.bumpers[i].rail);
}
void MiniGolfGame::penalizeHazard(size_t player,Vec2 position,bool water)
{
    auto& p=m_players[player];
    if(p.hazardTimer>0 || p.hazardImmunity>0 || p.respawnProtected || p.holedOut[m_currentHole]) return;
    p.strokes[m_currentHole]=static_cast<uint8_t>(std::min<int>(STROKE_CAP,p.strokes[m_currentHole]+1));
    p.hazardPosition=position;
    p.waterSplash=water;
    p.hazardTimer=0.8f;
    p.trail.clear();
    p.cupBounced=false;
    p.cupRejectTimer=0;
    freezeBody(p.ballBody);
    b2Body_Disable(p.ballBody); // No invisible ball collision during the splash.
    if(player==m_currentPlayer) {
        m_settleTimer=0;
        if(m_phase==Phase::Aiming) m_phase=Phase::BallInMotion;
    }
}
void MiniGolfGame::updateObstacles(float dt)
{
    const auto& h=m_course.holes[m_currentHole];
    returnDisplacedBalls(dt);
    for(auto& timer:m_bumperAnimation) timer=std::max(0.0f,timer-dt);
    for(size_t i=0;i<m_players.size();++i) {
        auto& p=m_players[i];
        if(!b2Body_IsValid(p.ballBody) || p.holedOut[m_currentHole]) continue;
        p.hazardImmunity=std::max(0.0f,p.hazardImmunity-dt);
        p.portalCooldown=std::max(0.0f,p.portalCooldown-dt);
        p.bumperCooldown=std::max(0.0f,p.bumperCooldown-dt);
        if(p.hazardTimer>0) {
            p.hazardTimer=std::max(0.0f,p.hazardTimer-dt);
            if(p.hazardTimer==0) {
                spawnBall(i,p.shotStart);
                p.hazardImmunity=1; // A moving hazard cannot immediately charge again.
                p.respawnProtected=true;
                if(p.strokes[m_currentHole]>=STROKE_CAP) p.finishedHole[m_currentHole]=true;
            }
            continue;
        }
        Vec2 pos;
        getBodyPositionPx(*m_world,p.ballBody,pos.x,pos.y);
        if(p.respawnProtected) {
            // If a rail carried a hazard onto the shot origin, do not charge
            // repeated penalties while the returned ball is still inside it.
            bool inside=false;
            for(const auto& s:h.surfaces)
                if(s.kind==SurfaceKind::Water && insidePatch(pos,s.center,s.size)) inside=true;
            for(const auto& l:h.lasers)
                if(insidePatch(pos,obstaclePosition(l.center,l.rail),
                    {l.size.x+2*BALL_RADIUS_PX,l.size.y+2*BALL_RADIUS_PX})) inside=true;
            if(!inside) p.respawnProtected=false;
        }
        for(const auto& s:h.surfaces)
            if(s.kind==SurfaceKind::Water && insidePatch(pos,s.center,s.size,4)) {
                penalizeHazard(i,pos,true); break;
            }
        if(p.hazardTimer>0) continue;
        for(const auto& l:h.lasers)
            if(laserActive(l,m_courseTime) && insidePatch(pos,obstaclePosition(l.center,l.rail),
                {l.size.x+2*BALL_RADIUS_PX,l.size.y+2*BALL_RADIUS_PX})) {
                penalizeHazard(i,pos,false); break;
            }
        if(p.hazardTimer>0) continue;
        for(size_t bi=0;bi<h.bumpers.size();++bi) {
            const auto& b=h.bumpers[bi];
            const auto center=obstaclePosition(b.center,b.rail);
            const float dx=pos.x-center.x,dy=pos.y-center.y,d=std::hypot(dx,dy);
            if(d>0 && d<=b.radius+BALL_RADIUS_PX+2 && p.bumperCooldown<=0) {
                float vx=0,vy=0; getBodyVelocityPx(*m_world,p.ballBody,vx,vy);
                const float nx=dx/d,ny=dy/d,out=vx*nx+vy*ny;
                const float kick=std::max(0.0f,b.kickSpeed-out);
                setBodyVelocityPx(*m_world,p.ballBody,vx+nx*kick,vy+ny*kick);
                p.bumperCooldown=0.16f;
                m_bumperAnimation[bi]=0.35f;
            }
        }
        bool inPortal=false;
        for(const auto& portal:h.portals) {
            const auto a=obstaclePosition(portal.center,portal.rail);
            if(std::hypot(pos.x-a.x,pos.y-a.y)<=h.cupRadius+BALL_RADIUS_PX) inPortal=true;
        }
        if(!inPortal) p.blockedPortalPair=-1;
        if(p.portalCooldown>0) continue;
        for(size_t k=0;k<h.portals.size();++k) {
            const auto& entry=h.portals[k];
            const auto a=obstaclePosition(entry.center,entry.rail);
            if(entry.pair==p.blockedPortalPair || std::hypot(pos.x-a.x,pos.y-a.y)>h.cupRadius) continue;
            size_t partner=h.portals.size(),matches=0;
            for(size_t j=0;j<h.portals.size();++j)
                if(j!=k && h.portals[j].pair==entry.pair) { partner=j; ++matches; }
            if(matches!=1) continue;
            const auto& exit=h.portals[partner];
            const auto center=obstaclePosition(exit.center,exit.rail);
            Vec2 velocity;
            getBodyVelocityPx(*m_world,p.ballBody,velocity.x,velocity.y);
            const auto entranceVelocity=railVelocity(h,entry.rail,m_courseTime,dt);
            const auto exitVelocity=railVelocity(h,exit.rail,m_courseTime,dt);
            const auto outgoing=portalExitVelocity(velocity,entranceVelocity,exitVelocity);
            Vec2 direction{velocity.x-entranceVelocity.x,velocity.y-entranceVelocity.y};
            float length=std::hypot(direction.x,direction.y);
            if(length<0.001f) {
                direction={pos.x-a.x,pos.y-a.y};
                length=std::hypot(direction.x,direction.y);
            }
            if(length<0.001f) direction={1,0};
            else { direction.x/=length; direction.y/=length; }
            const float clearance=h.cupRadius+BALL_RADIUS_PX+4;
            const Vec2 dest{center.x+direction.x*clearance,center.y+direction.y*clearance};
            // Blocked destinations never place a ball inside a wall.
            if(dest.x<h.areaTopLeft.x+BALL_RADIUS_PX || dest.x>h.areaBottomRight.x-BALL_RADIUS_PX ||
               dest.y<h.areaTopLeft.y+BALL_RADIUS_PX || dest.y>h.areaBottomRight.y-BALL_RADIUS_PX) continue;
            bool blocked=false;
            for(const auto& wall:h.walls)
                if(insidePatch(dest,obstaclePosition({wall.centerX,wall.centerY},wall.rail),
                    {wall.width+2*BALL_RADIUS_PX,wall.height+2*BALL_RADIUS_PX})) blocked=true;
            for(const auto& bumper:h.bumpers) {
                const auto bp=obstaclePosition(bumper.center,bumper.rail);
                if(std::hypot(dest.x-bp.x,dest.y-bp.y)<bumper.radius+BALL_RADIUS_PX) blocked=true;
            }
            for(size_t j=0;j<m_players.size();++j) {
                if(!m_options.ballCollisions || j==i || !b2Body_IsValid(m_players[j].ballBody) || m_players[j].hazardTimer>0) continue;
                Vec2 other;
                getBodyPositionPx(*m_world,m_players[j].ballBody,other.x,other.y);
                if(std::hypot(dest.x-other.x,dest.y-other.y)<2*BALL_RADIUS_PX) blocked=true;
            }
            if(blocked) continue;
            b2Body_SetTransform(p.ballBody,m_world->pixelsToMeters(dest.x,dest.y),b2Rot_identity);
            setBodyVelocityPx(*m_world,p.ballBody,outgoing.x,outgoing.y);
            p.portalCooldown=0.25f;
            p.blockedPortalPair=exit.pair;
            p.trail.clear(); // No line across the teleport, and orientation is preserved.
            p.cupBounced=false; p.cupRejectTimer=0;
            break;
        }
    }
}

void MiniGolfGame::renderObstacles()
{
    if(m_currentHole>=m_options.holeCount) return;
    const auto& h=m_course.holes[m_currentHole];
    const FrameID fid=getFrameId();
    auto circle=[&](Vec2 p,float radius,Color c,uint32_t z) {
        auto s=std::make_shared<RenderShape>(); s->m_type=ShapeType::Circle;
        m_camera.worldToScreen(p.x,p.y,s->m_x,s->m_y);
        s->m_width=2*m_camera.worldToScreenLength(radius); s->m_height=0;
        s->m_color=c; s->m_z=z; renderQueueAdd(fid,s);
    };
    auto box=[&](Vec2 p,Vec2 size,Color c,uint32_t z) {
        auto s=std::make_shared<RenderShape>(); s->m_type=ShapeType::Box;
        m_camera.worldToScreen(p.x-size.x/2,p.y-size.y/2,s->m_x,s->m_y);
        s->m_width=m_camera.worldToScreenLength(size.x); s->m_height=m_camera.worldToScreenLength(size.y);
        s->m_color=c; s->m_z=z; renderQueueAdd(fid,s);
    };
    auto line=[&](Vec2 a,Vec2 b,Color c,uint32_t z) {
        float ax,ay,bx,by; m_camera.worldToScreen(a.x,a.y,ax,ay); m_camera.worldToScreen(b.x,b.y,bx,by);
        const float length=std::hypot(bx-ax,by-ay);
        auto s=std::make_shared<RenderShape>(); s->m_type=ShapeType::Box;
        s->m_x=(ax+bx-length)/2; s->m_y=(ay+by)/2-1;
        s->m_width=length; s->m_height=2; s->m_rotation=std::atan2(by-ay,bx-ax);
        s->m_color=c; s->m_z=z; renderQueueAdd(fid,s);
    };
    auto rail=[&](Vec2 base,int index) {
        if(index<0 || static_cast<size_t>(index)>=h.rails.size()) return;
        const auto& r=h.rails[static_cast<size_t>(index)];
        for(size_t i=0;i<r.stops.size();++i) {
            const Vec2 a{base.x+r.stops[i].offset.x,base.y+r.stops[i].offset.y};
            circle(a,r.stops[i].pauseSeconds>0 ? 5.0f:3.0f,{135,145,130},CourseLayer::Rail);
            if(i+1<r.stops.size() || r.mode==RailMode::Loop) {
                const auto& q=r.stops[(i+1)%r.stops.size()].offset;
                line(a,{base.x+q.x,base.y+q.y},{105,125,105},CourseLayer::Rail);
            }
        }
    };
    for(const auto& s:h.surfaces) {
        const auto p=s.center;
        Color fill{},detail{};
        switch(s.kind) {
            case SurfaceKind::Sand: fill={190,160,85}; detail={225,200,125}; break;
            case SurfaceKind::Rough: fill={35,78,35}; detail={65,120,55}; break;
            case SurfaceKind::Ice: fill={150,205,220}; detail={220,245,250}; break;
            case SurfaceKind::Water: fill={30,100,170}; detail={85,180,230}; break;
        }
        const auto z=surfaceLayer(s.kind);
        box(p,s.size,fill,z);
        // Different patterns distinguish surfaces without relying only on colour.
        for(float y=-s.size.y/2+14;y<s.size.y/2-8;y+=28)
            for(float x=-s.size.x/2+14;x<s.size.x/2-8;x+=32) {
                const Vec2 q{p.x+x,p.y+y};
                if(s.kind==SurfaceKind::Sand) circle(q,1.5f,detail,z+1);
                else if(s.kind==SurfaceKind::Rough) line({q.x-2,q.y+4},{q.x+2,q.y-4},detail,z+1);
                else if(s.kind==SurfaceKind::Ice) line({q.x-6,q.y+5},{q.x+6,q.y-5},detail,z+1);
                else line({q.x-7,q.y},{q.x+7,q.y+2*std::sin(static_cast<float>(m_courseTime)*2+x)},detail,z+1);
            }
    }
    for(const auto& w:h.walls) if(w.showRail) rail({w.centerX,w.centerY},w.rail);
    rail(h.cupPos,h.cupRail);
    for(size_t i=0;i<h.bumpers.size();++i) {
        const auto& b=h.bumpers[i];
        rail(b.center,b.rail); const auto p=obstaclePosition(b.center,b.rail);
        const float hit=m_bumperAnimation[i]/0.35f;
        const float squash=std::sin(hit*3.14159265f);
        circle(p,b.radius,{130,65,25},CourseLayer::Bumper);
        circle(p,b.radius-4-5*squash,hit>0.6f ? Color{255,225,115}:Color{240,155,40},CourseLayer::Bumper+1);
        // Solid piston cap with a cross, distinct from hollow portal rings.
        box(p,{24,7},{130,65,25},CourseLayer::Bumper+2);
        box(p,{7,24},{130,65,25},CourseLayer::Bumper+2);
        if(hit>0) for(int ray=0;ray<8;++ray) {
            const float a=ray*0.78539816f;
            const float r=b.radius+3+12*(1-hit);
            line({p.x+std::cos(a)*r,p.y+std::sin(a)*r},
                 {p.x+std::cos(a)*(r+10*hit),p.y+std::sin(a)*(r+10*hit)},
                 {255,220,110},CourseLayer::Bumper+2);
        }
    }
    for(const auto& l:h.lasers) {
        rail(l.center,l.rail); const auto p=obstaclePosition(l.center,l.rail);
        const bool active=laserActive(l,m_courseTime);
        box(p,l.size,active ? Color{245,50,65}:Color{85,65,65},CourseLayer::Laser);
        if(active) box(p,{l.size.x,std::max(2.0f,l.size.y*0.3f)},{255,205,195},CourseLayer::Laser+1);
        circle({p.x-l.size.x/2,p.y},10,{90,90,100},CourseLayer::Laser+2);
        circle({p.x+l.size.x/2,p.y},10,{90,90,100},CourseLayer::Laser+2);
    }
    for(const auto& portal:h.portals) {
        rail(portal.center,portal.rail); const auto p=obstaclePosition(portal.center,portal.rail);
        circle(p,h.cupRadius,portal.color,CourseLayer::Portal); circle(p,h.cupRadius-6,{30,30,50},CourseLayer::Portal+1);
        for(int i=0;i<3;++i) {
            const float a=static_cast<float>(m_courseTime)*2+static_cast<float>(i)*2.094395f;
            circle({p.x+std::cos(a)*h.cupRadius*0.55f,p.y+std::sin(a)*h.cupRadius*0.55f},2,portal.color,CourseLayer::Portal+2);
        }
    }
    for(const auto& p:m_players) if(p.hazardTimer>0) {
        const float t=0.8f-p.hazardTimer;
        const Color c=p.waterSplash ? Color{160,220,255}:Color{255,140,110};
        for(int k=0;k<12;++k) {
            const float angle=static_cast<float>(k)*6.2831853f/12;
            const float r=12+t*55;
            const Vec2 tip{p.hazardPosition.x+std::cos(angle)*r,
                          p.hazardPosition.y+std::sin(angle)*r};
            if(p.waterSplash)
                circle({tip.x,tip.y-25*std::sin(t*3.92699f)},std::max(0.5f,4*(1-t/0.8f)),c,CourseLayer::Effect);
            else
                line({tip.x-std::cos(angle)*9,tip.y-std::sin(angle)*9},tip,c,CourseLayer::Effect);
        }
        if(!p.waterSplash && t<0.15f) circle(p.hazardPosition,18*(1-t/0.15f),{255,240,180},CourseLayer::Effect);
        auto penalty=std::make_shared<RenderText>();
        penalty->m_text="+1 stroke"; penalty->m_fontId=m_fontId;
        penalty->m_color=c; penalty->m_z=CourseLayer::Effect+1; penalty->m_rotation=0;
        penalty->m_scaleX=0.75f; penalty->m_scaleY=0.75f;
        m_camera.worldToScreen(p.hazardPosition.x-40,p.hazardPosition.y-45-t*20,penalty->m_x,penalty->m_y);
        renderQueueAdd(fid,penalty);
    }
    for(const auto& p:m_players) if(p.destructionTimer>0) {
        const float age=0.4f-p.destructionTimer;
        const float radius=12+age*90;
        for(int k=0;k<12;++k) {
            const float angle=k*6.2831853f/12;
            line({p.destructionPosition.x+std::cos(angle)*radius,
                  p.destructionPosition.y+std::sin(angle)*radius},
                 {p.destructionPosition.x+std::cos(angle)*(radius+12*p.destructionTimer/0.4f),
                  p.destructionPosition.y+std::sin(angle)*(radius+12*p.destructionTimer/0.4f)},
                 p.ballColor,CourseLayer::Effect);
        }
        if(age<0.12f) circle(p.destructionPosition,20*(1-age/0.12f),{255,240,180},CourseLayer::Effect+1);
    }
    // Always-visible legend makes the first few holes usable as a sampler.
    auto title=std::make_shared<RenderText>();
    title->m_text=std::to_string((m_options.startHole+m_currentHole)%9+1)+". "+h.name;
    title->m_fontId=m_fontId; title->m_color={235,235,215};
    title->m_x=30; title->m_y=25; title->m_z=200;
    title->m_scaleX=1; title->m_scaleY=1; title->m_rotation=0;
    renderQueueAdd(fid,title);
}
}
