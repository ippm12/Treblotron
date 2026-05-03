/**
 * physics_body.hpp
 *
 * High-level helpers that build Box2D bodies in pixel coordinates.
 * Each helper converts pixels to meters via PhysicsWorld::pixelsPerMeter
 * and returns a b2BodyId.
 *
 * userData is stored as a tagged-union pointer: the caller stuffs an
 * arbitrary game-specific pointer in via the userData parameter and
 * retrieves it later (e.g. from sensor events) via b2Body_GetUserData.
 * The PhysicsBodyKind tag lives in the high bits of the kindAndPointer
 * field via a wrapper struct so dispatchers can tell ball/wall/cup apart
 * cheaply.
 */

#ifndef GAME_LIB_PHYSICS_BODY_HPP
#define GAME_LIB_PHYSICS_BODY_HPP

#include "box2d/box2d.h"
#include "game_lib/box2d/physics_world.hpp"
#include <cstdint>


/** Generic kind tag. Games extend it as needed. */
enum class PhysicsBodyKind : uint8_t
{
    Unknown = 0,
    Wall    = 1,
    Ball    = 2,
    Cup     = 3,
    Custom  = 64    // games can use anything >= Custom
};


/**
 * UserData attached to bodies and shapes. Allocate one of these per body
 * and pass its address to the create helper; the body owns no memory.
 * The caller is responsible for keeping the userdata struct alive as
 * long as the body exists.
 */
struct PhysicsUserData
{
    PhysicsBodyKind kind    = PhysicsBodyKind::Unknown;
    void*           payload = nullptr;
};


/** Tunable surface properties. Sane mini-golf defaults. */
struct PhysicsMaterial
{
    float density     = 1.0f;
    float friction    = 0.3f;
    float restitution = 0.6f;  // moderately bouncy walls
};


/**
 * Static rectangular wall, axis-aligned in screen space. (px, py) is the
 * box centre; width/height are in pixels.
 */
b2BodyId createStaticBox(PhysicsWorld& world,
                         float pxCenter, float pyCenter,
                         float widthPx, float heightPx,
                         PhysicsUserData* userData = nullptr,
                         PhysicsMaterial mat = {});

/**
 * Dynamic circle. (pxCenter, pyCenter) is the body's start position in
 * pixels; radiusPx is the circle radius in pixels.
 *
 * `linearDamping` controls how quickly the body slows due to "rolling
 * friction" with the felt. Default (0.7f) settles a ball within a few
 * seconds; tune lower for icier surfaces.
 */
b2BodyId createDynamicCircle(PhysicsWorld& world,
                             float pxCenter, float pyCenter,
                             float radiusPx,
                             PhysicsUserData* userData = nullptr,
                             PhysicsMaterial mat = {},
                             float linearDamping = 0.7f);

/**
 * Static circular sensor. Generates b2SensorEvents but does not collide.
 * Use for a golf cup, scoring zone, etc.
 */
b2BodyId createStaticCircleSensor(PhysicsWorld& world,
                                  float pxCenter, float pyCenter,
                                  float radiusPx,
                                  PhysicsUserData* userData = nullptr);

/** Read body position back in pixel coordinates. */
void getBodyPositionPx(const PhysicsWorld& world, b2BodyId bodyId,
                       float& outPxX, float& outPxY);

/** Speed (linear velocity magnitude) in pixels per second. */
float getBodySpeedPx(const PhysicsWorld& world, b2BodyId bodyId);

/** Apply a linear impulse in pixel/sec units to the body's centre. */
void applyImpulsePxPerSec(const PhysicsWorld& world, b2BodyId bodyId,
                          float impulsePxX, float impulsePxY);

/** Stop a body and pin it in place (sets velocity to zero). */
void freezeBody(b2BodyId bodyId);


#endif // GAME_LIB_PHYSICS_BODY_HPP
