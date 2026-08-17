/**
 * vision.cpp
 *
 * Vision module state management. Owns the internal VisionSource
 * and delegates public API calls to it.
 */

#include "vision/vision.hpp"
#include "vision/vision_link.hpp"
#include "vision/vision_settings.hpp"
#include "vision_source.hpp"
#include "frame/render_queue.hpp"
#include "game_lib/components/render_shape.hpp"
#include "game_lib/components/render_text.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#ifdef DARTMATIC_USE_SIM
#include "sim_vision_source.hpp"
#endif
#ifdef DARTMATIC_USE_LOCAL
#include "local_vision_source.hpp"
#endif
#ifdef DARTMATIC_USE_NETWORK
#include "network_vision_source.hpp"
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

/**
 * Which vision source this binary was compiled with.
 *
 * Named on stdout at startup because the alternative is guessing. Every source
 * fails in its own way when it is the wrong one for the hardware, and it is easy
 * to spend a while debugging that before noticing you were running a stale
 * binary from a previous build directory.
 */
static constexpr const char* VISION_SOURCE_NAME =
#if   defined(DARTMATIC_USE_SIM)
    "sim (simulated darts, no cameras)";
#elif defined(DARTMATIC_USE_LOCAL)
    "local (local inference on this machine)";
#elif defined(DARTMATIC_USE_NETWORK)
    "network (cameras here, inference on a remote server)";
#else
    "none";
#endif


Status initializeVisionModule()
{
    LOG_INFO(VISION_LOG_ID, "Vision source compiled in: {}", VISION_SOURCE_NAME);

    // A missing address is not an error here — the app comes up regardless and
    // the settings overlay is how you fix it. Both loads happen before any
    // source is constructed, so a source that reads settings during init()
    // sees the user's values rather than the defaults.
    loadInferenceServerAddress();
    loadVisionSettings();

#ifdef DARTMATIC_USE_SIM
    f_visionSource = std::make_shared<SimVisionSource>();
#endif
#ifdef DARTMATIC_USE_LOCAL
    f_visionSource = std::make_shared<LocalVisionSource>();
#endif
#ifdef DARTMATIC_USE_NETWORK
    f_visionSource = std::make_shared<NetworkVisionSource>();
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


bool consumeBoardResetRequest()
{
    if(!f_visionSource) return false;
    return f_visionSource->consumeBoardResetRequest();
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


bool isVisionFailed()
{
    return f_visionSource && f_visionSource->isFailed();
}


float getVisionInitProgress()
{
    if(!f_visionSource) return 1.0f;
    return f_visionSource->getInitProgress();
}


uint64_t getVisionInitIteration()
{
    if(!f_visionSource) return 0;
    return f_visionSource->getInitIteration();
}


std::string getVisionInitStatus()
{
    if(!f_visionSource) return {};
    return f_visionSource->getInitStatus();
}


std::string getVisionDetectionStatus()
{
    if(!f_visionSource) return {};
    return f_visionSource->getDetectionStatus();
}


bool visionHasDetector()
{
#if defined(DARTMATIC_USE_LOCAL) || defined(DARTMATIC_USE_NETWORK)
    return true;
#else
    return false;
#endif
}


bool visionUsesRemoteServer()
{
#ifdef DARTMATIC_USE_NETWORK
    return true;
#else
    return false;
#endif
}


Status saveVisionCapture(const std::string& outputDir)
{
    // Prefer the remote save: the server has the frames that were scored, and
    // the warps derived from them. Only fall back locally when there is no
    // server in the picture at all.
    if(f_visionSource && f_visionSource->requestCapture()) return STATUS_OK;
    return saveAllCameraFrames(outputDir);
}


std::string consumeVisionCaptureResult()
{
    if(!f_visionSource) return {};
    return f_visionSource->consumeCaptureResult();
}


VisionLinkState getVisionLinkState()
{
    if(!f_visionSource) return VisionLinkState::NotApplicable;
    return f_visionSource->getLinkState();
}


std::string getVisionLinkDetail()
{
    if(!f_visionSource) return {};
    return f_visionSource->getLinkDetail();
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

    const bool failed = isVisionFailed();
    const std::string status = getVisionInitStatus();

    // ---- Title ---------------------------------------------------------
    if(f_loadingTitleFont != INVALID_FONT_ID)
    {
        // The network source is not building anything — it is waiting on a
        // server. Saying "building vision model" there sends people looking
        // for a problem that does not exist.
#ifdef DARTMATIC_USE_NETWORK
        const std::string busyTitle = "Connecting to inference server";
#else
        const std::string busyTitle = "Building vision model";
#endif
        const std::string titleText = failed ? std::string("Vision init failed") : busyTitle;
        const Color titleColor = failed ? Color{240, 130, 110} : Color{230, 230, 240};

        auto title = std::make_shared<RenderText>();
        title->m_text     = titleText;
        title->m_color    = titleColor;
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

    if(failed)
    {
        // ---- Failure screen: error message + dismiss hint ------------
        // No progress bar, no spinner. The status string already starts
        // with "Failed: <reason>" — we strip the prefix for a cleaner
        // visual since the title above already conveys the failure.
        if(f_loadingBodyFont != INVALID_FONT_ID && !status.empty())
        {
            std::string msg = status;
            const std::string prefix = "Failed: ";
            if(msg.compare(0, prefix.size(), prefix) == 0)
            {
                msg = msg.substr(prefix.size());
            }

            auto err = std::make_shared<RenderText>();
            err->m_text     = msg;
            err->m_color    = {230, 200, 195};
            err->m_fontId   = f_loadingBodyFont;
            err->m_rotation = 0.0f;
            err->m_scaleX   = 1.0f;
            err->m_scaleY   = 1.0f;
            const float approxW = static_cast<float>(msg.size()) * 13.0f;
            err->m_x        = CENTER_X - approxW * 0.5f;
            err->m_y        = 460.0f;
            err->m_z        = 10;
            renderQueueAdd(FRAME, err);

            const std::string hint = "Close the window to quit.";
            auto dismiss = std::make_shared<RenderText>();
            dismiss->m_text     = hint;
            dismiss->m_color    = {140, 150, 165};
            dismiss->m_fontId   = f_loadingBodyFont;
            dismiss->m_rotation = 0.0f;
            dismiss->m_scaleX   = 1.0f;
            dismiss->m_scaleY   = 1.0f;
            const float hintW = static_cast<float>(hint.size()) * 13.0f;
            dismiss->m_x        = CENTER_X - hintW * 0.5f;
            dismiss->m_y        = 540.0f;
            dismiss->m_z        = 10;
            renderQueueAdd(FRAME, dismiss);
        }

        renderQueueDrawFlush(FRAME);
        presentFrame(FRAME);
        return;
    }

    if(f_loadingBodyFont != INVALID_FONT_ID)
    {
        auto sub = std::make_shared<RenderText>();
#ifdef DARTMATIC_USE_NETWORK
        sub->m_text     = "(the game will start anyway — press F1 to change the address)";
#else
        sub->m_text     = "(first-run only — subsequent launches are instant)";
#endif
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
    // Single monotonic 0 → 100% bar that advances at C++ phase markers
    // in local_vision_source.cpp::buildThreadMain. The backend progress
    // monitor doesn't touch the bar — it only ticks the iteration
    // counter shown below, so the bar can't bounce backwards as TRT
    // cycles through internal phases.
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

    // ---- Status text + iteration counter + elapsed time --------------
    if(f_loadingBodyFont != INVALID_FONT_ID)
    {
        if(!status.empty())
        {
            auto phase = std::make_shared<RenderText>();
            phase->m_text     = status;
            phase->m_color    = {200, 210, 225};
            phase->m_fontId   = f_loadingBodyFont;
            phase->m_rotation = 0.0f;
            phase->m_scaleX   = 1.0f;
            phase->m_scaleY   = 1.0f;
            const float approxW = static_cast<float>(status.size()) * 13.0f;
            phase->m_x        = CENTER_X - approxW * 0.5f;
            phase->m_y        = BAR_Y + BAR_H + 24.0f;
            phase->m_z        = 10;
            renderQueueAdd(FRAME, phase);
        }

        const uint64_t iter = getVisionInitIteration();
        const int elapsedSec = static_cast<int>(f_loadingElapsedSec);
        char activityBuf[64];
        std::snprintf(activityBuf, sizeof(activityBuf),
                      "iter %llu  -  %d:%02d elapsed",
                      static_cast<unsigned long long>(iter),
                      elapsedSec / 60, elapsedSec % 60);

        auto activity = std::make_shared<RenderText>();
        activity->m_text     = activityBuf;
        activity->m_color    = {130, 140, 155};
        activity->m_fontId   = f_loadingBodyFont;
        activity->m_rotation = 0.0f;
        activity->m_scaleX   = 1.0f;
        activity->m_scaleY   = 1.0f;
        const float approxW = static_cast<float>(strlen(activityBuf)) * 13.0f;
        activity->m_x        = CENTER_X - approxW * 0.5f;
        activity->m_y        = BAR_Y + BAR_H + 64.0f;
        activity->m_z        = 10;
        renderQueueAdd(FRAME, activity);
    }

    // ---- Spinner: 8 dots arranged in a ring, one "leading" -----------
    // Leading dot rotates at ~1 revolution per second; trailing dots
    // fade behind it. Keeps the screen feeling alive even when the
    // progress bar sits at the same value for a while during a long
    // internal TRT phase.
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
