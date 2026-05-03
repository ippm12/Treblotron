/**
 * physics_camera.hpp
 *
 * 2D camera that maps a world rectangle (in pixel space) into a viewport
 * rectangle (in screen pixels). Supports follow-with-deadzone and
 * clamp-to-bounds so a course never reveals beyond its extents.
 *
 * The camera does not zoom in v1 — `pixelsPerWorldPixel` defaults to 1.
 * Bumping it to 2.0f doubles the on-screen size. Negative is undefined.
 */

#ifndef GAME_LIB_PHYSICS_CAMERA_HPP
#define GAME_LIB_PHYSICS_CAMERA_HPP


class PhysicsCamera
{
    public:
        /** Set the on-screen rectangle the camera draws into. */
        void setViewport(float screenX, float screenY,
                         float screenW, float screenH);

        /** Set the world bounds (axis-aligned) the camera will clamp to. */
        void setWorldBounds(float minX, float minY,
                            float maxX, float maxY);

        /** Set the centre of the camera in world-pixel coordinates. */
        void setCenter(float wx, float wy);

        /** Move centre toward (tx, ty) but only if it leaves the deadzone. */
        void follow(float tx, float ty, float deadzonePx = 80.0f);

        /** Convert a world-pixel position to a screen-pixel position. */
        void worldToScreen(float wx, float wy,
                           float& outSx, float& outSy) const;

        /** Convert a screen-pixel position to a world-pixel position. */
        void screenToWorld(float sx, float sy,
                           float& outWx, float& outWy) const;

        /** Scale a world-pixel length to a screen-pixel length. */
        float worldToScreenLength(float lengthPx) const;

        float viewportX() const { return m_viewX; }
        float viewportY() const { return m_viewY; }
        float viewportW() const { return m_viewW; }
        float viewportH() const { return m_viewH; }
        float zoom()      const { return m_pixelsPerWorldPixel; }
        void  setZoom(float z) { m_pixelsPerWorldPixel = z; }

    private:
        void clampCenter();

        // Viewport on screen
        float m_viewX = 0.0f, m_viewY = 0.0f;
        float m_viewW = 1920.0f, m_viewH = 1080.0f;

        // World bounds
        float m_boundsMinX = 0.0f, m_boundsMinY = 0.0f;
        float m_boundsMaxX = 1920.0f, m_boundsMaxY = 1080.0f;
        bool  m_hasBounds = false;

        // Centre in world-pixel space
        float m_centerX = 960.0f, m_centerY = 540.0f;

        float m_pixelsPerWorldPixel = 1.0f;
};


#endif // GAME_LIB_PHYSICS_CAMERA_HPP
