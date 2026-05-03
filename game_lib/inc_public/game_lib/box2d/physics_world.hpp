/**
 * physics_world.hpp
 *
 * Thin RAII wrapper over a Box2D v3 b2WorldId. Owns the world handle,
 * exposes a step() that advances simulation, and stores the
 * pixels-per-meter scale that body builders and renderers share.
 *
 * Top-down games (mini golf, billiards, pinball) default gravity to
 * zero — set it explicitly via setGravity() if you need it.
 */

#ifndef GAME_LIB_PHYSICS_WORLD_HPP
#define GAME_LIB_PHYSICS_WORLD_HPP

#include "box2d/box2d.h"

class PhysicsWorld
{
    public:
        PhysicsWorld();
        ~PhysicsWorld();

        PhysicsWorld(const PhysicsWorld&)            = delete;
        PhysicsWorld& operator=(const PhysicsWorld&) = delete;

        /** Advance the world by deltaTime seconds. */
        void step(float deltaTime, int subStepCount = 4);

        /** Override gravity (default zero — top-down). */
        void setGravity(float gx, float gy);

        b2WorldId id() const { return m_worldId; }

        /** Pixels-per-meter scale used by body builders and renderers. */
        float pixelsPerMeter() const { return m_pixelsPerMeter; }
        void  setPixelsPerMeter(float ppm) { m_pixelsPerMeter = ppm; }

        /** Helpers for unit conversions. */
        float metersToPixels(float meters) const { return meters * m_pixelsPerMeter; }
        float pixelsToMeters(float pixels) const { return pixels / m_pixelsPerMeter; }
        b2Vec2 pixelsToMeters(float px, float py) const
        {
            return { px / m_pixelsPerMeter, py / m_pixelsPerMeter };
        }

    private:
        b2WorldId m_worldId        = b2_nullWorldId;
        float     m_pixelsPerMeter = 100.0f;  // 1 m == 100 px
};

#endif // GAME_LIB_PHYSICS_WORLD_HPP
