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
#ifdef DARTLENS_USE_HAILO
#include "hailo_vision_source.hpp"
#endif

#include <memory>


// ============================================================================
// Module state
// ============================================================================

static VisionSourcePtr f_visionSource;

// Stored at module level so callbacks registered before the VisionSource
// exists (e.g., game manager inits before vision) are not lost.
static DartLandedCallback   f_onDartLanded;
static DartPositionCallback f_onDartPositionCalculated;


// ============================================================================
// Lifecycle
// ============================================================================

Status initializeVisionModule()
{
#ifdef DARTLENS_USE_SIM
    f_visionSource = std::make_shared<SimVisionSource>();
#endif
#ifdef DARTLENS_USE_HAILO
    f_visionSource = std::make_shared<HailoVisionSource>();
#endif

    if(f_visionSource)
    {
        // Apply any callbacks registered before the vision source was created
        f_visionSource->setCallbacks(f_onDartLanded, f_onDartPositionCalculated);

        Status stat = f_visionSource->init();
        if(IS_STATUS_NOT_OK(stat))
        {
            // init() may have partially initialized internal resources (e.g.
            // brought the camera system up before a later step failed). Run
            // shutdown so they get torn down cleanly — otherwise their
            // destructors fire during process exit and terminate the program.
            f_visionSource->shutdown();
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


void resetVisionDarts()
{
    if(f_visionSource)
    {
        f_visionSource->resetDarts();
    }
}


bool getLatestVisionHeatmap(std::vector<float>& out, uint32_t& width, uint32_t& height)
{
    if(!f_visionSource) return false;
    return f_visionSource->getLatestHeatmap(out, width, height);
}


// ============================================================================
// Game connection
// ============================================================================

void setVisionCallbacks(DartLandedCallback onDartLanded,
                        DartPositionCallback onDartPositionCalculated)
{
    f_onDartLanded = onDartLanded;
    f_onDartPositionCalculated = onDartPositionCalculated;

    if(f_visionSource)
    {
        f_visionSource->setCallbacks(f_onDartLanded, f_onDartPositionCalculated);
    }
}
