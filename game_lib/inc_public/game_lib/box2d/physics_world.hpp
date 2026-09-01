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

        /**
         * Minimum closing speed, in pixels/sec measured along the contact
         * normal, at which restitution is applied. Below it Box2D treats the
         * impact as fully inelastic — the body slides along the surface
         * instead of bouncing off it.
         *
         * Box2D's default is 1 m/s, which at a pixelsPerMeter of 100 means
         * anything approaching a surface slower than 100 px/s silently stops
         * bouncing. That is a lot of gameplay speed for a top-down game, and
         * it shows up as glancing hits skating along a wall. The threshold is
         * still worth keeping small-but-nonzero: it is what stops a body that
         * has come to rest against a surface from buzzing forever.
         *
         * Call this *after* setPixelsPerMeter() — it converts to Box2D's
         * metres using the scale in effect at the time of the call.
         */
        void setRestitutionThresholdPx(float pxPerSec);

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
