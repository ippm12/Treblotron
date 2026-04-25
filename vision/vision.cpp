/**
 * vision.cpp
 *
 * Vision module state management. Owns the internal VisionSource
 * and delegates public API calls to it.
 */

#include "vision/vision.hpp"
#include "vision_source.hpp"
#include "frame/render_queue.hpp"
#include "game_lib/components/render_shape.hpp"
#include "game_lib/components/render_text.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#ifdef DARTLENS_USE_SIM
#include "sim_vision_source.hpp"
#endif
#ifdef DARTLENS_USE_HAILO
#include "hailo_vision_source.hpp"
#endif
#ifdef DARTLENS_USE_TENSORRT
#include "tensorrt_vision_source.hpp"
#endif

#include <memory>


// Forward-declaration — defined alongside the loading-screen renderer below
// but called from shutdownVisionModule up top.
static void unloadLoadingFonts();


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
#ifdef DARTLENS_USE_TENSORRT
    f_visionSource = std::make_shared<TensorRTVisionSource>();
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
    unloadLoadingFonts();
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


// ============================================================================
// Async-init state + loading-screen rendering
// ============================================================================

bool isVisionInitializing()
{
    return f_visionSource && f_visionSource->isInitializing();
}


float getVisionInitProgress()
{
    if(!f_visionSource) return 1.0f;
    return f_visionSource->getInitProgress();
}


std::string getVisionInitStatus()
{
    if(!f_visionSource) return {};
    return f_visionSource->getInitStatus();
}


// Loading-screen state. Fonts are loaded lazily on first use and kept
// around until shutdownVisionModule(). Elapsed/spinner-phase state lives
// here too so the loading loop can stay stateless.
static FontID f_loadingTitleFont = INVALID_FONT_ID;
static FontID f_loadingBodyFont  = INVALID_FONT_ID;
static float  f_loadingElapsedSec = 0.0f;

static void ensureLoadingFontsLoaded()
{
    if(f_loadingTitleFont == INVALID_FONT_ID)
    {
        f_loadingTitleFont = loadFont("assets/fonts/Roboto-Regular.ttf", 56.0f);
    }
    if(f_loadingBodyFont == INVALID_FONT_ID)
    {
        f_loadingBodyFont = loadFont("assets/fonts/Roboto-Regular.ttf", 28.0f);
    }
}

static void unloadLoadingFonts()
{
    if(f_loadingTitleFont != INVALID_FONT_ID)
    {
        unloadFont(f_loadingTitleFont);
        f_loadingTitleFont = INVALID_FONT_ID;
    }
    if(f_loadingBodyFont != INVALID_FONT_ID)
    {
        unloadFont(f_loadingBodyFont);
        f_loadingBodyFont = INVALID_FONT_ID;
    }
}


void presentVisionLoadingFrame(float deltaTime)
{
    // Logical coordinates — game_manager set SDL_SetRenderLogicalPresentation
    // to 1920x1080 before we got here, so we draw in that space regardless
    // of the physical monitor resolution.
    constexpr FrameID FRAME  = 0;
    constexpr float   SCREEN_W = 1920.0f;
    constexpr float   CENTER_X = SCREEN_W * 0.5f;

    ensureLoadingFontsLoaded();

    f_loadingElapsedSec += deltaTime;

    // Dark slate background.
    renderQueueClearFrame(FRAME, 18, 22, 32);

    // ---- Title ---------------------------------------------------------
    if(f_loadingTitleFont != INVALID_FONT_ID)
    {
        auto title = std::make_shared<RenderText>();
        title->m_text     = "Building vision model";
        title->m_color    = {230, 230, 240};
        title->m_fontId   = f_loadingTitleFont;
        title->m_rotation = 0.0f;
        title->m_scaleX   = 1.0f;
        title->m_scaleY   = 1.0f;
        // Approximate width for centering: ~0.5 px per pt-size per char.
        // Good enough — we're not trying for pixel perfection.
        const float approxW = static_cast<float>(title->m_text.size()) * 28.0f;
        title->m_x        = CENTER_X - approxW * 0.5f;
        title->m_y        = 340.0f;
        title->m_z        = 10;
        renderQueueAdd(FRAME, title);
    }

    if(f_loadingBodyFont != INVALID_FONT_ID)
    {
        auto sub = std::make_shared<RenderText>();
        sub->m_text     = "(first-run only — subsequent launches are instant)";
        sub->m_color    = {150, 160, 180};
        sub->m_fontId   = f_loadingBodyFont;
        sub->m_rotation = 0.0f;
        sub->m_scaleX   = 1.0f;
        sub->m_scaleY   = 1.0f;
        const float approxW = static_cast<float>(sub->m_text.size()) * 13.0f;
        sub->m_x        = CENTER_X - approxW * 0.5f;
        sub->m_y        = 410.0f;
        sub->m_z        = 10;
        renderQueueAdd(FRAME, sub);
    }

    // ---- Progress bar -------------------------------------------------
    constexpr float BAR_W    = 960.0f;
    constexpr float BAR_H    = 32.0f;
    constexpr float BAR_X    = CENTER_X - BAR_W * 0.5f;
    constexpr float BAR_Y    = 540.0f;

    auto barBg = std::make_shared<RenderShape>();
    barBg->m_type   = ShapeType::Box;
    barBg->m_color  = {40, 46, 58};
    barBg->m_x      = BAR_X;
    barBg->m_y      = BAR_Y;
    barBg->m_width  = BAR_W;
    barBg->m_height = BAR_H;
    barBg->m_z      = 5;
    renderQueueAdd(FRAME, barBg);

    const float progress = std::clamp(getVisionInitProgress(), 0.0f, 1.0f);
    auto barFg = std::make_shared<RenderShape>();
    barFg->m_type   = ShapeType::Box;
    barFg->m_color  = {80, 170, 230};
    barFg->m_x      = BAR_X;
    barFg->m_y      = BAR_Y;
    barFg->m_width  = BAR_W * progress;
    barFg->m_height = BAR_H;
    barFg->m_z      = 6;
    renderQueueAdd(FRAME, barFg);

    // ---- Progress percentage + status text ----------------------------
    if(f_loadingBodyFont != INVALID_FONT_ID)
    {
        char pctBuf[32];
        std::snprintf(pctBuf, sizeof(pctBuf), "%d%%",
                      static_cast<int>(std::round(progress * 100.0f)));

        auto pct = std::make_shared<RenderText>();
        pct->m_text     = pctBuf;
        pct->m_color    = {220, 225, 235};
        pct->m_fontId   = f_loadingBodyFont;
        pct->m_rotation = 0.0f;
        pct->m_scaleX   = 1.0f;
        pct->m_scaleY   = 1.0f;
        pct->m_x        = CENTER_X - 40.0f;
        pct->m_y        = BAR_Y + BAR_H + 20.0f;
        pct->m_z        = 10;
        renderQueueAdd(FRAME, pct);

        const std::string status = getVisionInitStatus();
        if(!status.empty())
        {
            auto phase = std::make_shared<RenderText>();
            phase->m_text     = status;
            phase->m_color    = {180, 190, 205};
            phase->m_fontId   = f_loadingBodyFont;
            phase->m_rotation = 0.0f;
            phase->m_scaleX   = 1.0f;
            phase->m_scaleY   = 1.0f;
            const float approxW = static_cast<float>(status.size()) * 13.0f;
            phase->m_x        = CENTER_X - approxW * 0.5f;
            phase->m_y        = BAR_Y + BAR_H + 60.0f;
            phase->m_z        = 10;
            renderQueueAdd(FRAME, phase);
        }

        // Elapsed time (minutes:seconds).
        const int elapsedSec = static_cast<int>(f_loadingElapsedSec);
        char elapsedBuf[32];
        std::snprintf(elapsedBuf, sizeof(elapsedBuf), "%d:%02d elapsed",
                      elapsedSec / 60, elapsedSec % 60);

        auto elapsed = std::make_shared<RenderText>();
        elapsed->m_text     = elapsedBuf;
        elapsed->m_color    = {120, 130, 145};
        elapsed->m_fontId   = f_loadingBodyFont;
        elapsed->m_rotation = 0.0f;
        elapsed->m_scaleX   = 1.0f;
        elapsed->m_scaleY   = 1.0f;
        const float approxW = static_cast<float>(strlen(elapsedBuf)) * 13.0f;
        elapsed->m_x        = CENTER_X - approxW * 0.5f;
        elapsed->m_y        = BAR_Y + BAR_H + 100.0f;
        elapsed->m_z        = 10;
        renderQueueAdd(FRAME, elapsed);
    }

    // ---- Spinner: 8 dots arranged in a ring, one "leading" -----------
    // Leading dot rotates at ~1 revolution per second; trailing dots
    // fade behind it. Keeps the screen feeling alive even if TRT
    // progress sits on the same percentage for a while.
    constexpr int   SPINNER_DOTS    = 8;
    constexpr float SPINNER_RADIUS  = 42.0f;
    constexpr float SPINNER_CX      = CENTER_X;
    constexpr float SPINNER_CY      = 770.0f;
    constexpr float DOT_DIAMETER    = 18.0f;
    constexpr float ROTATIONS_PER_S = 0.8f;

    const float leadAngle = f_loadingElapsedSec * ROTATIONS_PER_S * 2.0f * static_cast<float>(M_PI);
    for(int i = 0; i < SPINNER_DOTS; i++)
    {
        const float dotAngle = static_cast<float>(i) * (2.0f * static_cast<float>(M_PI) / SPINNER_DOTS);
        // Relative phase: 0 = the "leading" bright dot; grows as we go
        // back around the ring.
        float phase = dotAngle - leadAngle;
        while(phase < 0.0f) phase += 2.0f * static_cast<float>(M_PI);
        while(phase >= 2.0f * static_cast<float>(M_PI)) phase -= 2.0f * static_cast<float>(M_PI);
        const float brightness = 0.25f + 0.75f * (1.0f - phase / (2.0f * static_cast<float>(M_PI)));
        const uint8_t v = static_cast<uint8_t>(std::round(brightness * 220.0f));

        auto dot = std::make_shared<RenderShape>();
        dot->m_type  = ShapeType::Circle;
        dot->m_color = {v, static_cast<uint8_t>(v * 0.9f), static_cast<uint8_t>(std::min(255, v + 30))};
        dot->m_x     = SPINNER_CX + SPINNER_RADIUS * std::cos(dotAngle);
        dot->m_y     = SPINNER_CY + SPINNER_RADIUS * std::sin(dotAngle);
        dot->m_width = DOT_DIAMETER;
        dot->m_z     = 8;
        renderQueueAdd(FRAME, dot);
    }

    renderQueueDrawFlush(FRAME);
    presentFrame(FRAME);
}
