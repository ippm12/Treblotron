/**
 * physics_camera.cpp
 */

#include "game_lib/box2d/physics_camera.hpp"
#include <algorithm>
#include <cmath>


void PhysicsCamera::setViewport(float screenX, float screenY,
                                float screenW, float screenH)
{
    m_viewX = screenX;
    m_viewY = screenY;
    m_viewW = screenW;
    m_viewH = screenH;
    clampCenter();
}


void PhysicsCamera::setWorldBounds(float minX, float minY,
                                   float maxX, float maxY)
{
    m_boundsMinX = minX;
    m_boundsMinY = minY;
    m_boundsMaxX = maxX;
    m_boundsMaxY = maxY;
    m_hasBounds  = true;
    clampCenter();
}


void PhysicsCamera::setCenter(float wx, float wy)
{
    m_centerX = wx;
    m_centerY = wy;
    clampCenter();
}


void PhysicsCamera::follow(float tx, float ty, float deadzonePx)
{
    const float dx = tx - m_centerX;
    const float dy = ty - m_centerY;
    if(std::fabs(dx) > deadzonePx)
    {
        m_centerX += (dx > 0.0f) ? (dx - deadzonePx) : (dx + deadzonePx);
    }
    if(std::fabs(dy) > deadzonePx)
    {
        m_centerY += (dy > 0.0f) ? (dy - deadzonePx) : (dy + deadzonePx);
    }
    clampCenter();
}


void PhysicsCamera::worldToScreen(float wx, float wy,
                                  float& outSx, float& outSy) const
{
    outSx = m_viewX + 0.5f * m_viewW + (wx - m_centerX) * m_pixelsPerWorldPixel;
    outSy = m_viewY + 0.5f * m_viewH + (wy - m_centerY) * m_pixelsPerWorldPixel;
}


void PhysicsCamera::screenToWorld(float sx, float sy,
                                  float& outWx, float& outWy) const
{
    outWx = m_centerX + (sx - m_viewX - 0.5f * m_viewW) / m_pixelsPerWorldPixel;
    outWy = m_centerY + (sy - m_viewY - 0.5f * m_viewH) / m_pixelsPerWorldPixel;
}


float PhysicsCamera::worldToScreenLength(float lengthPx) const
{
    return lengthPx * m_pixelsPerWorldPixel;
}


void PhysicsCamera::clampCenter()
{
    if(!m_hasBounds) return;
    const float halfW = 0.5f * m_viewW / m_pixelsPerWorldPixel;
    const float halfH = 0.5f * m_viewH / m_pixelsPerWorldPixel;
    const float worldW = m_boundsMaxX - m_boundsMinX;
    const float worldH = m_boundsMaxY - m_boundsMinY;

    if(2.0f * halfW >= worldW)
    {
        // Viewport wider than world — pin to centre.
        m_centerX = 0.5f * (m_boundsMinX + m_boundsMaxX);
    }
    else
    {
        m_centerX = std::clamp(m_centerX,
                               m_boundsMinX + halfW,
                               m_boundsMaxX - halfW);
    }
    if(2.0f * halfH >= worldH)
    {
        m_centerY = 0.5f * (m_boundsMinY + m_boundsMaxY);
    }
    else
    {
        m_centerY = std::clamp(m_centerY,
                               m_boundsMinY + halfH,
                               m_boundsMaxY - halfH);
    }
}
