/**
 * physics_body.cpp
 */

#include "game_lib/box2d/physics_body.hpp"
#include <cmath>


b2BodyId createStaticBox(PhysicsWorld& world,
                         float pxCenter, float pyCenter,
                         float widthPx, float heightPx,
                         PhysicsUserData* userData,
                         PhysicsMaterial mat)
{
    const float ppm = world.pixelsPerMeter();

    b2BodyDef bodyDef = b2DefaultBodyDef();
    bodyDef.type     = b2_staticBody;
    bodyDef.position = { pxCenter / ppm, pyCenter / ppm };
    bodyDef.userData = userData;
    b2BodyId body = b2CreateBody(world.id(), &bodyDef);

    b2ShapeDef shapeDef = b2DefaultShapeDef();
    shapeDef.density               = mat.density;
    shapeDef.material.friction     = mat.friction;
    shapeDef.material.restitution  = mat.restitution;
    shapeDef.userData              = userData;

    b2Polygon poly = b2MakeBox(0.5f * widthPx / ppm, 0.5f * heightPx / ppm);
    b2CreatePolygonShape(body, &shapeDef, &poly);
    return body;
}


b2BodyId createDynamicCircle(PhysicsWorld& world,
                             float pxCenter, float pyCenter,
                             float radiusPx,
                             PhysicsUserData* userData,
                             PhysicsMaterial mat,
                             float linearDamping)
{
    const float ppm = world.pixelsPerMeter();

    b2BodyDef bodyDef = b2DefaultBodyDef();
    bodyDef.type            = b2_dynamicBody;
    bodyDef.position        = { pxCenter / ppm, pyCenter / ppm };
    bodyDef.linearDamping   = linearDamping;
    bodyDef.angularDamping  = 0.5f;
    bodyDef.userData        = userData;
    b2BodyId body = b2CreateBody(world.id(), &bodyDef);

    b2ShapeDef shapeDef = b2DefaultShapeDef();
    shapeDef.density              = mat.density;
    shapeDef.material.friction    = mat.friction;
    shapeDef.material.restitution = mat.restitution;
    shapeDef.userData             = userData;
    shapeDef.enableSensorEvents   = true;  // ball needs to fire cup-sensor events

    b2Circle circle;
    circle.center = { 0.0f, 0.0f };
    circle.radius = radiusPx / ppm;
    b2CreateCircleShape(body, &shapeDef, &circle);
    return body;
}


b2BodyId createStaticCircleSensor(PhysicsWorld& world,
                                  float pxCenter, float pyCenter,
                                  float radiusPx,
                                  PhysicsUserData* userData)
{
    const float ppm = world.pixelsPerMeter();

    b2BodyDef bodyDef = b2DefaultBodyDef();
    bodyDef.type     = b2_staticBody;
    bodyDef.position = { pxCenter / ppm, pyCenter / ppm };
    bodyDef.userData = userData;
    b2BodyId body = b2CreateBody(world.id(), &bodyDef);

    b2ShapeDef shapeDef = b2DefaultShapeDef();
    shapeDef.isSensor           = true;
    shapeDef.enableSensorEvents = true;
    shapeDef.userData           = userData;

    b2Circle circle;
    circle.center = { 0.0f, 0.0f };
    circle.radius = radiusPx / ppm;
    b2CreateCircleShape(body, &shapeDef, &circle);
    return body;
}


void getBodyPositionPx(const PhysicsWorld& world, b2BodyId bodyId,
                       float& outPxX, float& outPxY)
{
    const float ppm = world.pixelsPerMeter();
    b2Vec2 p = b2Body_GetPosition(bodyId);
    outPxX = p.x * ppm;
    outPxY = p.y * ppm;
}


float getBodySpeedPx(const PhysicsWorld& world, b2BodyId bodyId)
{
    const float ppm = world.pixelsPerMeter();
    b2Vec2 v = b2Body_GetLinearVelocity(bodyId);
    return std::sqrt(v.x * v.x + v.y * v.y) * ppm;
}


void applyImpulsePxPerSec(const PhysicsWorld& world, b2BodyId bodyId,
                          float impulsePxX, float impulsePxY)
{
    const float ppm = world.pixelsPerMeter();
    // Impulse units = mass * (m/s). Caller passes (px/s); divide by ppm to
    // get the m/s component, multiply by mass for impulse magnitude.
    const float mass = b2Body_GetMass(bodyId);
    b2Vec2 impulse = {
        (impulsePxX / ppm) * mass,
        (impulsePxY / ppm) * mass
    };
    b2Body_ApplyLinearImpulseToCenter(bodyId, impulse, true);
}


void freezeBody(b2BodyId bodyId)
{
    b2Body_SetLinearVelocity(bodyId, b2Vec2{ 0.0f, 0.0f });
    b2Body_SetAngularVelocity(bodyId, 0.0f);
}
