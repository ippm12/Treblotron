/**
 * physics_world.cpp
 */

#include "game_lib/box2d/physics_world.hpp"
#include "debug/crash_reporting.hpp"
#include <cstdio>


PhysicsWorld::PhysicsWorld()
{
#ifdef _WIN32
    b2SetAssertFcn([](const char* condition,const char* file,int line) -> int {
        char reason[512];
        std::snprintf(reason,sizeof(reason),"Box2D assertion: %s at %s:%d",condition,file,line);
        reportFatalCrash(reason);
    });
#endif
    b2WorldDef def = b2DefaultWorldDef();
    def.gravity = { 0.0f, 0.0f };  // top-down default
    m_worldId = b2CreateWorld(&def);
}


PhysicsWorld::~PhysicsWorld()
{
    if(b2World_IsValid(m_worldId))
    {
        b2DestroyWorld(m_worldId);
        m_worldId = b2_nullWorldId;
    }
}


void PhysicsWorld::step(float deltaTime, int subStepCount)
{
    if(!b2World_IsValid(m_worldId)) return;
    b2World_Step(m_worldId, deltaTime, subStepCount);
}


void PhysicsWorld::setGravity(float gx, float gy)
{
    if(!b2World_IsValid(m_worldId)) return;
    b2World_SetGravity(m_worldId, b2Vec2{ gx, gy });
}


void PhysicsWorld::setRestitutionThresholdPx(float pxPerSec)
{
    if(!b2World_IsValid(m_worldId)) return;
    b2World_SetRestitutionThreshold(m_worldId, pxPerSec / m_pixelsPerMeter);
}
