/**
 * vision.cpp
 *
 * Vision module state management. Owns the internal VisionSource
 * and delegates public API calls to it.
 */

#include "vision/vision.hpp"
#include "vision_source.hpp"

#ifdef DARTLENS_USE_SIM
#include "sim_vision_source.hpp"
#endif

#include <memory>


// ============================================================================
// Module state
// ============================================================================

static VisionSourcePtr f_visionSource;


// ============================================================================
// Lifecycle
// ============================================================================

Status initializeVisionModule()
{
#ifdef DARTLENS_USE_SIM
    f_visionSource = std::make_shared<SimVisionSource>();
#endif

    if(f_visionSource)
    {
        Status stat = f_visionSource->init();
        if(IS_STATUS_NOT_OK(stat))
        {
            f_visionSource = nullptr;
            return stat;
        }
        LOG_INFO(VISION_LOG_ID, "Vision module initialized");
    }
    else
    {
        LOG_INFO(VISION_LOG_ID, "Vision module initialized (no vision source)");
    }

    return STATUS_OK;
}


void shutdownVisionModule()
{
    if(f_visionSource)
    {
        f_visionSource->shutdown();
        f_visionSource = nullptr;
    }
    LOG_INFO(VISION_LOG_ID, "Vision module shut down");
}


// ============================================================================
// Per-frame update
// ============================================================================

void tickVision(float deltaTime)
{
    if(f_visionSource)
    {
        f_visionSource->tick(deltaTime);
    }
}


// ============================================================================
// Board state
// ============================================================================

bool isBoardClear()
{
    if(!f_visionSource)
    {
        return true;
    }
    return f_visionSource->isBoardClear();
}


// ============================================================================
// Game connection
// ============================================================================

void setVisionGame(GamePtr game)
{
    if(f_visionSource)
    {
        f_visionSource->setGame(game);
    }
}
