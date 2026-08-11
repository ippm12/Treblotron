/**
 * game_manager.cpp
 *
 * Implementation of the game manager module.
 */

#include <SDL3/SDL.h>
#include <SDL3/SDL_keycode.h>
#include <SDL3/SDL_gamepad.h>
#include <SDL3_ttf/SDL_ttf.h>
#include <string>
#include <vector>
#include "common_inc.hpp"
#include "frame/frame.hpp"
#include "frame/render_queue.hpp"
#include "game_lib/game_manager.hpp"
#include "game_lib/components/render_shape.hpp"
#include "game_lib/components/render_text.hpp"
#include "games/main_menu.hpp"
#include "game_manager_class.hpp"
#include "vision/vision.hpp"
#include "vision/vision_link.hpp"
#include "players/players.hpp"


// ============================================================================
// Game base class
// ============================================================================

Game::Game(const std::string& pName)
    : m_name(pName)
{
}


Game::~Game()
{
}


std::string Game::getName() const
{
    return m_name;
}


FrameID Game::getFrameId() const
{
    return m_frameId;
}


void Game::onDartLanded()
{
    m_dartLandedCount.fetch_add(1, std::memory_order_release);
}


void Game::onDartPositionCalculated(float angle, float normalizedRadius)
{
    std::lock_guard<std::mutex> lock(m_dartPositionMutex);
    m_dartPositionQueue.push({angle, normalizedRadius});
}


uint32_t Game::consumeDartLandedCount()
{
    return m_dartLandedCount.exchange(0, std::memory_order_acquire);
}


bool Game::popDartPosition(DartPosition& out)
{
    std::lock_guard<std::mutex> lock(m_dartPositionMutex);
    if(m_dartPositionQueue.empty())
    {
        return false;
    }
    out = m_dartPositionQueue.front();
    m_dartPositionQueue.pop();
    return true;
}


void Game::onTurnSkipped()
{
    // Fire onMissedThrow() until the bar reports we're no longer in PlayerTurn
    // with throws remaining. Capped at 16 iterations as a safety against a
    // misbehaving override (e.g. one whose throwsRemaining doesn't decrement).
    for(int safety = 0; safety < 16; safety++)
    {
        GameBarInfo info = getBarInfo();
        if(info.state != GameState::PlayerTurn || info.throwsRemaining == 0) break;
        onMissedThrow();
    }
}


// ============================================================================
// GameManager class
// ============================================================================

static constexpr size_t WINDOW_WIDTH  = 1920;
static constexpr size_t WINDOW_HEIGHT = 1080;


GameManager::GameManager()
    : m_initialized(false), m_currentGame(nullptr),
      m_lastTickNs(0), m_frameId(INVALID_FRAME_ID), m_barFontId(INVALID_FONT_ID),
      m_pauseFontId(INVALID_FONT_ID)
#ifdef DARTLENS_SHOW_FPS
    , m_fpsFontId(INVALID_FONT_ID), m_fpsAccumulator(0.0f), m_fpsFrameCount(0), m_fpsDisplay(0)
#endif
{
}


GameManager::~GameManager()
{
    shutdown();
}


Status GameManager::initialize()
{
    if(m_initialized)
    {
        LOG_ERROR(GAME_MANAGER_LOG_ID, "Game Manager already initialized");
        return STATUS_ERROR_INVALID_STATE;
    }

    // Create the main window
    Status stat = createNewFrame("DartLens", WINDOW_WIDTH, WINDOW_HEIGHT, m_frameId);
    if(IS_STATUS_NOT_OK(stat))
    {
        LOG_ERROR(GAME_MANAGER_LOG_ID, "Failed to create main window");
        return stat;
    }

    // On real-camera builds (Hailo / TensorRT) we run on a dedicated cabinet
    // display, so come up fullscreen. The sim build is used for development
    // on a desktop where a windowed mode is more useful.
#ifndef DARTLENS_USE_SIM
    setFrameFullscreen(m_frameId, true);
#endif

    // Set logical presentation so all game code uses 1920x1080 coordinates.
    // SDL auto-scales to the actual monitor resolution (letterboxed).
    SDL_SetRenderLogicalPresentation(getFrameRenderer(m_frameId),
        WINDOW_WIDTH, WINDOW_HEIGHT, SDL_LOGICAL_PRESENTATION_LETTERBOX);

    // Load the status bar font
    m_barFontId = loadFont("assets/fonts/Roboto-Regular.ttf", 63.0f);
    if(m_barFontId == INVALID_FONT_ID)
    {
        LOG_ERROR(GAME_MANAGER_LOG_ID, "Failed to load bar font");
        deleteFrame(m_frameId);
        m_frameId = INVALID_FRAME_ID;
        return STATUS_ERROR_GENERIC;
    }

    // Load the pause menu font
    m_pauseFontId = loadFont("assets/fonts/Roboto-Regular.ttf", 42.0f);
    if(m_pauseFontId == INVALID_FONT_ID)
    {
        LOG_WARNING(GAME_MANAGER_LOG_ID, "Failed to load pause menu font");
    }

    // The address editor needs its fonts before it can draw anything. Without
    // this it still paints its full-width backdrop but every key and label is
    // invisible, which looks like a bare dark box swallowing the lower screen.
    {
        const FontID kbFont = (m_pauseFontId != INVALID_FONT_ID) ? m_pauseFontId : m_barFontId;
        m_settingsKeyboard.init(kbFont, kbFont);
    }

    // Shortcut to the connection settings. Deliberately only live while the
    // link is down: when everything is working these belong to the game, and
    // settings is still reachable from the main menu.
    constexpr uint32_t SETTINGS_KEY = SDLK_F1;
    constexpr uint8_t  SETTINGS_GAMEPAD_BUTTON = SDL_GAMEPAD_BUTTON_BACK;

    // Register input handlers — GameManager owns these and forwards to games
    registerFrameKeyHandler(m_frameId, [this](FrameID, uint32_t keycode, bool pressed) {
        if(!pressed || !m_currentGame) return;

        if(m_settingsOpen)
        {
            handleSettingsKey(keycode);
            return;
        }

        // Only bound while the link is down. When everything is working this
        // key belongs to the game; the settings page is still reachable from
        // the main menu, so nothing is lost by not claiming it permanently.
        if(keycode == SETTINGS_KEY && getVisionLinkState() == VisionLinkState::Disconnected)
        {
            openSettings();
            return;
        }

        if(m_paused)
        {
            handlePauseKey(keycode);
            return;
        }

        if(keycode == SDLK_ESCAPE && m_currentGame->isPauseable())
        {
            m_paused = true;
            m_pauseCursor = 0;
            m_pauseStatus.clear();
            return;
        }

        // Manual missed-throw button — works in any game whose current
        // turn still has throws remaining. Pass through to the game's
        // own keyhandler in any other state so games can use 'M' freely.
        if(keycode == SDLK_M)
        {
            const GameBarInfo info = m_currentGame->getBarInfo();
            if(info.state == GameState::PlayerTurn && info.throwsRemaining > 0)
            {
                m_currentGame->onMissedThrow();
                return;
            }
        }

        m_currentGame->onKeyDown(keycode);
    });

    registerFrameGamepadButtonHandler(m_frameId, [this](FrameID, uint8_t button, bool pressed) {
        if(!pressed || !m_currentGame) return;

        if(m_settingsOpen)
        {
            switch(button)
            {
                case SDL_GAMEPAD_BUTTON_DPAD_UP:    handleSettingsKey(SDLK_UP);     break;
                case SDL_GAMEPAD_BUTTON_DPAD_DOWN:  handleSettingsKey(SDLK_DOWN);   break;
                case SDL_GAMEPAD_BUTTON_DPAD_LEFT:  handleSettingsKey(SDLK_LEFT);   break;
                case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: handleSettingsKey(SDLK_RIGHT);  break;
                case SDL_GAMEPAD_BUTTON_SOUTH:      handleSettingsKey(SDLK_RETURN); break;
                case SDL_GAMEPAD_BUTTON_EAST:       handleSettingsKey(SDLK_ESCAPE); break;
                case SDL_GAMEPAD_BUTTON_BACK:       handleSettingsKey(SDLK_ESCAPE); break;
                default: break;
            }
            return;
        }

        if(button == SETTINGS_GAMEPAD_BUTTON
        && getVisionLinkState() == VisionLinkState::Disconnected)
        {
            openSettings();
            return;
        }

        if(m_paused)
        {
            switch(button)
            {
                case SDL_GAMEPAD_BUTTON_DPAD_UP:    handlePauseKey(SDLK_UP);     break;
                case SDL_GAMEPAD_BUTTON_DPAD_DOWN:  handlePauseKey(SDLK_DOWN);   break;
                case SDL_GAMEPAD_BUTTON_SOUTH:      handlePauseKey(SDLK_RETURN); break;
                case SDL_GAMEPAD_BUTTON_START:       handlePauseKey(SDLK_ESCAPE); break;
                default: break;
            }
            return;
        }

        if(button == SDL_GAMEPAD_BUTTON_START && m_currentGame->isPauseable())
        {
            m_paused = true;
            m_pauseCursor = 0;
            m_pauseStatus.clear();
            return;
        }

        // Manual missed-throw button (X on Xbox controller). Same gating
        // as the keyboard 'M' — only fires during PlayerTurn with throws
        // left, otherwise passes through.
        if(button == SDL_GAMEPAD_BUTTON_WEST)
        {
            const GameBarInfo info = m_currentGame->getBarInfo();
            if(info.state == GameState::PlayerTurn && info.throwsRemaining > 0)
            {
                m_currentGame->onMissedThrow();
                return;
            }
        }

        m_currentGame->onGamepadButton(button, pressed);
    });

    registerFrameTextHandler(m_frameId, [this](FrameID, const char* text) {
        if(m_settingsOpen)
        {
            // Physical keyboard types straight into the address field, so a
            // Pi with a keyboard attached doesn't have to peck at the on-screen
            // one. The virtual keyboard stays visible for controller use.
            // The on-screen keyboard keeps its own buffer; the physical path
            // types into m_settingsBuffer.
            if(m_settingsKeyboard.isOpen()) m_settingsKeyboard.handleTextInput(text);
            else                            handleSettingsText(text);
            return;
        }
        if(m_paused || !m_currentGame) return;
        m_currentGame->onTextInput(text);
    });

    // Load input hint icons for the pause instruction
    m_inputHints.init();

    m_currentGame = nullptr;
    m_lastTickNs = SDL_GetTicksNS();

#ifdef DARTLENS_SHOW_FPS
    m_fpsFontId = loadFont("assets/fonts/Roboto-Regular.ttf", 24.0f);
    if(m_fpsFontId == INVALID_FONT_ID)
    {
        LOG_WARNING(GAME_MANAGER_LOG_ID, "Failed to load FPS font — FPS display disabled");
    }
#endif

    m_initialized = true;

    LOG_INFO(GAME_MANAGER_LOG_ID, "Game Manager initialized");
    return STATUS_OK;
}


void GameManager::shutdown()
{
    if(!m_initialized)
    {
        return;
    }

    // Unregister input handlers
    unregisterFrameTextHandler(m_frameId);
    unregisterFrameGamepadButtonHandler(m_frameId);
    unregisterFrameKeyHandler(m_frameId);

    // Unload current game
    if(m_currentGame != nullptr)
    {
        m_currentGame->shutdown();
        m_currentGame = nullptr;
    }

    // Unload input hint icons
    m_inputHints.shutdown();

    // Unload pause menu font
    if(m_pauseFontId != INVALID_FONT_ID)
    {
        unloadFont(m_pauseFontId);
        m_pauseFontId = INVALID_FONT_ID;
    }

#ifdef DARTLENS_SHOW_FPS
    if(m_fpsFontId != INVALID_FONT_ID)
    {
        unloadFont(m_fpsFontId);
        m_fpsFontId = INVALID_FONT_ID;
    }
#endif

    // Unload bar font
    if(m_barFontId != INVALID_FONT_ID)
    {
        unloadFont(m_barFontId);
        m_barFontId = INVALID_FONT_ID;
    }

    // Close the main window
    if(m_frameId != INVALID_FRAME_ID)
    {
        deleteFrame(m_frameId);
        m_frameId = INVALID_FRAME_ID;
    }

    m_initialized = false;

    LOG_INFO(GAME_MANAGER_LOG_ID, "Game Manager shut down");
}


Status GameManager::loadGame(GamePtr game, std::function<GamePtr()> restartFactory)
{
    if(!m_initialized)
    {
        LOG_ERROR(GAME_MANAGER_LOG_ID, "Game Manager not initialized");
        return STATUS_ERROR_NOT_INIT;
    }

    // Unload current game if one is active
    Status stat = unloadGame();
    if(IS_STATUS_NOT_OK(stat))
    {
        return stat;
    }

    // Reset pause state
    m_paused = false;
    m_pauseCursor = 0;
    m_pauseStatus.clear();
    m_gameFactory = restartFactory;

    // Load new game
    if(game != nullptr)
    {
        // Validate player count against game maximum
        uint8_t playerCount = getPlayerCount();
        uint8_t maxPlayers  = game->getMaxPlayers();
        if(maxPlayers > 0 && playerCount > maxPlayers)
        {
            LOG_ERROR(GAME_MANAGER_LOG_ID, "Too many players ({}) for game '{}' (max {})",
                playerCount, game->getName(), maxPlayers);
            return STATUS_ERROR_TOO_MANY_PLAYERS;
        }

        LOG_INFO(GAME_MANAGER_LOG_ID, "Loading game: {}", game->getName());
        stat = game->init(m_frameId);
        if(IS_STATUS_NOT_OK(stat))
        {
            LOG_ERROR(GAME_MANAGER_LOG_ID, "Failed to init game: {}", game->getName());
            return stat;
        }
        m_currentGame = game;

        // Connect vision callbacks to the new game
        setVisionCallbacks(
            [game]() { game->onDartLanded(); },
            [game](float angle, float normalizedRadius) {
                game->onDartPositionCalculated(angle, normalizedRadius);
            }
        );

        // Route mouse clicks on the game window to the game
        registerFrameClickHandler(m_frameId,
            [game](FrameID, float x, float y, uint8_t button)
            {
                game->onMouseClick(x, y, button);
            });
    }

    // Reset delta time so first tick after load is not huge
    m_lastTickNs = SDL_GetTicksNS();

    return STATUS_OK;
}


Status GameManager::unloadGame()
{
    if(!m_initialized)
    {
        LOG_ERROR(GAME_MANAGER_LOG_ID, "Game Manager not initialized");
        return STATUS_ERROR_NOT_INIT;
    }

    if(m_currentGame != nullptr)
    {
        LOG_INFO(GAME_MANAGER_LOG_ID, "Unloading game: {}", m_currentGame->getName());

        // Disconnect vision callbacks
        setVisionCallbacks(nullptr, nullptr);

        // Disconnect mouse click handler
        unregisterFrameClickHandler(m_frameId);

        m_currentGame->shutdown();
        m_currentGame = nullptr;
    }

    return STATUS_OK;
}


Status GameManager::restartCurrentGame()
{
    if(!m_initialized)
    {
        return STATUS_ERROR_NOT_INIT;
    }

    if(!m_gameFactory)
    {
        LOG_WARNING(GAME_MANAGER_LOG_ID, "No restart factory available");
        return STATUS_ERROR_INVALID_STATE;
    }

    auto factory = m_gameFactory;
    GamePtr newGame = factory();
    if(!newGame)
    {
        return STATUS_ERROR_GENERIC;
    }

    return loadGame(newGame, factory);
}


void GameManager::tick()
{
    if(!m_initialized)
    {
        return;
    }

    // Calculate delta time
    uint64_t nowNs = SDL_GetTicksNS();
    float deltaTime = static_cast<float>(nowNs - m_lastTickNs) / 1'000'000'000.0f;
    m_lastTickNs = nowNs;

    // Clamp delta to avoid spiral of death (e.g., after breakpoint)
    if(deltaTime > 0.25f)
    {
        deltaTime = 0.25f;
    }

    // Tick vision source (processes pending throw delays, renders sim window)
    tickVision(deltaTime);

    // Update and render current game
    if(m_currentGame != nullptr)
    {
        // Detect a turn-skip signal. Two events count:
        //   1) Board went from not-clear to clear (user pulled darts mid-
        //      turn, real or sim).
        //   2) The vision source raised an explicit reset request (sim's
        //      Collect button — fires even when the board was already
        //      empty, so the player can still end an unstarted turn).
        // If either fires while a turn has throws remaining, treat all
        // remaining throws as misses via Game::onTurnSkipped(). Skip
        // while paused so a dart-pull during the pause menu is harmless.
        const bool boardClearNow  = isBoardClear();
        const bool boardClearEdge = boardClearNow && !m_lastBoardClear;
        const bool resetRequested = consumeBoardResetRequest();

        if(!m_paused && (boardClearEdge || resetRequested))
        {
            const GameBarInfo info = m_currentGame->getBarInfo();
            if(info.state == GameState::PlayerTurn && info.throwsRemaining > 0)
            {
                LOG_INFO(GAME_MANAGER_LOG_ID,
                         "Board reset signalled with {} throws remaining — "
                         "treating as turn skip", info.throwsRemaining);
                m_currentGame->onTurnSkipped();
            }
        }
        m_lastBoardClear = boardClearNow;

        if(!m_paused)
        {
            m_currentGame->update(deltaTime);
        }

        renderQueueClearFrame(m_frameId, 40, 40, 40);
        m_currentGame->render();

        renderLinkIndicator();

        if(m_settingsOpen)
        {
            renderSettings();
        }
        else if(m_paused)
        {
            renderPauseMenu();
        }
        else
        {
            enqueueBar(m_currentGame->getBarInfo());
        }

#ifdef DARTLENS_SHOW_FPS
        enqueueFps(deltaTime);
#endif
        renderQueueDrawFlush(m_frameId);
        presentFrame(m_frameId);
    }
}


// ============================================================================
// Pause menu
// ============================================================================

namespace
{

enum class PauseAction : uint8_t { Resume, Restart, SaveCapture, MainMenu };

struct PauseOption { const char* label; PauseAction action; };

std::vector<PauseOption> buildPauseOptions(bool hasRestart)
{
    std::vector<PauseOption> opts;
    opts.push_back({"Resume", PauseAction::Resume});
    if(hasRestart) opts.push_back({"Restart", PauseAction::Restart});
    opts.push_back({"Save Capture", PauseAction::SaveCapture});
    opts.push_back({"Main Menu", PauseAction::MainMenu});
    return opts;
}

}  // namespace


void GameManager::handlePauseKey(uint32_t keycode)
{
    bool hasRestart = (m_gameFactory != nullptr);
    auto options = buildPauseOptions(hasRestart);
    uint8_t optionCount = static_cast<uint8_t>(options.size());

    switch(keycode)
    {
        case SDLK_UP:
            m_pauseCursor = (m_pauseCursor == 0) ? optionCount - 1 : m_pauseCursor - 1;
            break;
        case SDLK_DOWN:
            m_pauseCursor = (m_pauseCursor >= optionCount - 1) ? 0 : m_pauseCursor + 1;
            break;
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
        {
            switch(options[m_pauseCursor].action)
            {
                case PauseAction::Resume:
                    m_paused = false;
                    m_pauseStatus.clear();
                    break;
                case PauseAction::Restart:
                    m_paused = false;
                    m_pauseStatus.clear();
                    restartCurrentGame();
                    break;
                case PauseAction::SaveCapture:
                {
                    // On a remote build this only *requests* the save; the
                    // server writes the frames it actually scored and reports
                    // back, which renderPauseMenu picks up below.
                    Status stat = saveVisionCapture("./captures");
                    m_pauseStatus = IS_STATUS_OK(stat)
                        ? "Saving capture..."
                        : "Failed to save capture";
                    break;
                }
                case PauseAction::MainMenu:
                    m_paused = false;
                    m_pauseStatus.clear();
                    loadGame(std::make_shared<MainMenu>());
                    break;
            }
            break;
        }
        case SDLK_ESCAPE:
            m_paused = false;
            m_pauseStatus.clear();
            break;
        default:
            break;
    }
}


static constexpr uint32_t PAUSE_OVERLAY_Z = 500;

void GameManager::renderPauseMenu()
{
    // The remote capture result arrives on the client thread, not from the call
    // that asked for it, so pick it up here and replace the placeholder.
    const std::string captureResult = consumeVisionCaptureResult();
    if(!captureResult.empty()) m_pauseStatus = captureResult;

    // Dark overlay
    auto overlay = std::make_shared<RenderShape>();
    overlay->m_type   = ShapeType::Box;
    overlay->m_color  = {20, 20, 20};
    overlay->m_x      = 0.0f;
    overlay->m_y      = 0.0f;
    overlay->m_z      = PAUSE_OVERLAY_Z;
    overlay->m_width  = 1920.0f;
    overlay->m_height = 1080.0f;
    renderQueueAdd(m_frameId, overlay);

    // Center panel
    bool hasRestart = (m_gameFactory != nullptr);
    auto options = buildPauseOptions(hasRestart);
    uint8_t optionCount = static_cast<uint8_t>(options.size());
    float panelW = 600.0f;
    float rowH   = 75.0f;
    float statusH = m_pauseStatus.empty() ? 0.0f : 50.0f;
    float panelH = 120.0f + optionCount * rowH + 30.0f + statusH;
    float panelX = (1920.0f - panelW) * 0.5f;
    float panelY = (1080.0f - panelH) * 0.5f;

    auto panel = std::make_shared<RenderShape>();
    panel->m_type   = ShapeType::Box;
    panel->m_color  = {50, 50, 55};
    panel->m_x      = panelX;
    panel->m_y      = panelY;
    panel->m_z      = PAUSE_OVERLAY_Z + 1;
    panel->m_width  = panelW;
    panel->m_height = panelH;
    renderQueueAdd(m_frameId, panel);

    // Title
    TTF_Font* barFont = getFont(m_barFontId);
    const char* title = "Paused";
    int titleW = 0, titleH = 0;
    if(barFont)
    {
        TTF_GetStringSize(barFont, title, 0, &titleW, &titleH);
    }

    auto titleText = std::make_shared<RenderText>();
    titleText->m_text     = title;
    titleText->m_color    = {200, 200, 200};
    titleText->m_fontId   = m_barFontId;
    titleText->m_rotation = 0.0f;
    titleText->m_scaleX   = 1.0f;
    titleText->m_scaleY   = 1.0f;
    titleText->m_x        = panelX + (panelW - titleW) * 0.5f;
    titleText->m_y        = panelY + 30.0f;
    titleText->m_z        = PAUSE_OVERLAY_Z + 2;
    renderQueueAdd(m_frameId, titleText);

    float optRowW = 390.0f;
    float optStartY = panelY + 120.0f;
    FontID optFontId = (m_pauseFontId != INVALID_FONT_ID) ? m_pauseFontId : m_barFontId;
    TTF_Font* optFont = getFont(optFontId);

    for(uint8_t i = 0; i < optionCount; i++)
    {
        float rowY = optStartY + i * rowH;
        bool isSelected = (i == m_pauseCursor);
        float rowX = panelX + (panelW - optRowW) * 0.5f;

        if(isSelected)
        {
            auto bg = std::make_shared<RenderShape>();
            bg->m_type   = ShapeType::Box;
            bg->m_color  = {70, 70, 120};
            bg->m_x      = rowX;
            bg->m_y      = rowY;
            bg->m_z      = PAUSE_OVERLAY_Z + 2;
            bg->m_width  = optRowW;
            bg->m_height = rowH - 9.0f;
            renderQueueAdd(m_frameId, bg);
        }

        int optW = 0, optH = 0;
        if(optFont)
        {
            TTF_GetStringSize(optFont, options[i].label, 0, &optW, &optH);
        }

        auto optText = std::make_shared<RenderText>();
        optText->m_text     = options[i].label;
        optText->m_color    = isSelected ? Color{255, 255, 255} : Color{160, 160, 170};
        optText->m_fontId   = optFontId;
        optText->m_rotation = 0.0f;
        optText->m_scaleX   = 1.0f;
        optText->m_scaleY   = 1.0f;
        optText->m_x        = rowX + (optRowW - optW) * 0.5f;
        optText->m_y        = rowY + (rowH - 9.0f - optH) * 0.5f;
        optText->m_z        = PAUSE_OVERLAY_Z + 3;
        renderQueueAdd(m_frameId, optText);
    }

    // Transient status line (e.g. after a Save Capture action)
    if(!m_pauseStatus.empty())
    {
        int statusW = 0, statusH2 = 0;
        if(optFont)
        {
            TTF_GetStringSize(optFont, m_pauseStatus.c_str(), 0, &statusW, &statusH2);
        }

        auto statusText = std::make_shared<RenderText>();
        statusText->m_text     = m_pauseStatus;
        statusText->m_color    = {200, 220, 200};
        statusText->m_fontId   = optFontId;
        statusText->m_rotation = 0.0f;
        statusText->m_scaleX   = 1.0f;
        statusText->m_scaleY   = 1.0f;
        statusText->m_x        = panelX + (panelW - statusW) * 0.5f;
        statusText->m_y        = optStartY + optionCount * rowH + 10.0f;
        statusText->m_z        = PAUSE_OVERLAY_Z + 3;
        renderQueueAdd(m_frameId, statusText);
    }
}


// ============================================================================
// Status bar rendering
// ============================================================================

static constexpr float    BAR_Y       = 930.0f;
static constexpr float    BAR_HEIGHT  = 150.0f;
static constexpr float    BAR_WIDTH   = 1920.0f;
static constexpr uint32_t BAR_Z       = UINT32_MAX - 10;
static constexpr float    BAR_TEXT_Y  = BAR_Y + 45.0f;
static constexpr float    BAR_PAD_X   = 30.0f;
static constexpr float    DART_CIRCLE_DIAMETER = 53.0f;
static constexpr float    DART_CIRCLE_SPACING  = 78.0f;
static constexpr float    DART_CIRCLE_GAP      = 60.0f;  // Gap between name text and first dart circle
static constexpr float    DART_CIRCLE_Y        = BAR_Y + BAR_HEIGHT * 0.5f;
static constexpr size_t   MAX_PLAYER_NAME_DISPLAY = 12;  // Max characters to display for player name


void GameManager::enqueueBar(const GameBarInfo& info)
{
    // Bar background
    auto bg = std::make_shared<RenderShape>();
    bg->m_type   = ShapeType::Box;
    bg->m_color  = {30, 30, 30};
    bg->m_x      = 0.0f;
    bg->m_y      = BAR_Y;
    bg->m_z      = BAR_Z;
    bg->m_width  = BAR_WIDTH;
    bg->m_height = BAR_HEIGHT;
    renderQueueAdd(m_frameId, bg);

    if(info.state == GameState::PlayerTurn)
    {
        // Player name (left), truncated with "..." if too long
        std::string displayName = info.playerName;
        if(displayName.length() > MAX_PLAYER_NAME_DISPLAY)
        {
            displayName = displayName.substr(0, MAX_PLAYER_NAME_DISPLAY - 3) + "...";
        }

        auto name = std::make_shared<RenderText>();
        name->m_text     = displayName;
        name->m_color    = {255, 255, 255};
        name->m_fontId   = m_barFontId;
        name->m_rotation = 0.0f;
        name->m_scaleX   = 1.0f;
        name->m_scaleY   = 1.0f;
        name->m_x        = BAR_PAD_X;
        name->m_y        = BAR_TEXT_Y;
        name->m_z        = BAR_Z + 1;
        renderQueueAdd(m_frameId, name);

        // Measure the rendered name width to position dart circles after it
        float nameEndX = BAR_PAD_X;
        TTF_Font* barFont = getFont(m_barFontId);
        if(barFont)
        {
            int textW = 0;
            int textH = 0;
            TTF_GetStringSize(barFont, displayName.c_str(), 0, &textW, &textH);
            nameEndX += static_cast<float>(textW);
        }

        // Dart circles (filled, one per remaining throw, clamped to 0-3)
        uint8_t throwsToShow = info.throwsRemaining > 3 ? 3 : info.throwsRemaining;
        float dartStartX = nameEndX + DART_CIRCLE_GAP;
        for(uint8_t i = 0; i < throwsToShow; i++)
        {
            auto circle = std::make_shared<RenderShape>();
            circle->m_type  = ShapeType::Circle;
            circle->m_color = {255, 255, 255};
            circle->m_x     = dartStartX + i * DART_CIRCLE_SPACING;
            circle->m_y     = DART_CIRCLE_Y;
            circle->m_z     = BAR_Z + 1;
            circle->m_width = DART_CIRCLE_DIAMETER;
            renderQueueAdd(m_frameId, circle);
        }

        // Status text (right)
        if(!info.statusText.empty())
        {
            auto status = std::make_shared<RenderText>();
            status->m_text     = info.statusText;
            status->m_color    = {255, 255, 255};
            status->m_fontId   = m_barFontId;
            status->m_rotation = 0.0f;
            status->m_scaleX   = 1.0f;
            status->m_scaleY   = 1.0f;
            status->m_x        = BAR_WIDTH - BAR_PAD_X - info.statusText.length() * 33.0f;
            status->m_y        = BAR_TEXT_Y;
            status->m_z        = BAR_Z + 1;
            renderQueueAdd(m_frameId, status);
        }
    }
    else if(info.state == GameState::CollectDarts)
    {
        // Player name (left)
        std::string displayName = info.playerName;
        if(displayName.length() > MAX_PLAYER_NAME_DISPLAY)
        {
            displayName = displayName.substr(0, MAX_PLAYER_NAME_DISPLAY - 3) + "...";
        }

        auto name = std::make_shared<RenderText>();
        name->m_text     = displayName;
        name->m_color    = {255, 255, 255};
        name->m_fontId   = m_barFontId;
        name->m_rotation = 0.0f;
        name->m_scaleX   = 1.0f;
        name->m_scaleY   = 1.0f;
        name->m_x        = BAR_PAD_X;
        name->m_y        = BAR_TEXT_Y;
        name->m_z        = BAR_Z + 1;
        renderQueueAdd(m_frameId, name);

        // Status text (right)
        if(!info.statusText.empty())
        {
            auto status = std::make_shared<RenderText>();
            status->m_text     = info.statusText;
            status->m_color    = {255, 220, 50};
            status->m_fontId   = m_barFontId;
            status->m_rotation = 0.0f;
            status->m_scaleX   = 1.0f;
            status->m_scaleY   = 1.0f;
            status->m_x        = BAR_WIDTH - BAR_PAD_X - info.statusText.length() * 33.0f;
            status->m_y        = BAR_TEXT_Y;
            status->m_z        = BAR_Z + 1;
            renderQueueAdd(m_frameId, status);
        }
    }
    // Blank: background only

    // Pause hint (shown for all pauseable games, regardless of state)
    if(m_currentGame && m_currentGame->isPauseable() && m_pauseFontId != INVALID_FONT_ID)
    {
        m_inputHints.render(m_frameId, m_pauseFontId, BAR_WIDTH - 225.0f,
                            BAR_Y - 60.0f, BAR_Z, {
            {SDLK_ESCAPE, SDL_GAMEPAD_BUTTON_START, "pause"}
        });
    }
}


#ifdef DARTLENS_SHOW_FPS
static constexpr float    FPS_UPDATE_INTERVAL = 0.5f;
static constexpr float    FPS_X               = 1830.0f;
static constexpr float    FPS_Y               = 6.0f;
static constexpr uint32_t FPS_Z               = 200;

void GameManager::enqueueFps(float deltaTime)
{
    if(m_fpsFontId == INVALID_FONT_ID) return;

    m_fpsFrameCount++;
    m_fpsAccumulator += deltaTime;

    if(m_fpsAccumulator >= FPS_UPDATE_INTERVAL)
    {
        m_fpsDisplay = static_cast<int>(
            static_cast<float>(m_fpsFrameCount) / m_fpsAccumulator + 0.5f);
        m_fpsFrameCount  = 0;
        m_fpsAccumulator = 0.0f;
    }

    std::string fpsStr = std::to_string(m_fpsDisplay) + " fps";

    auto text = std::make_shared<RenderText>();
    text->m_text     = fpsStr;
    text->m_color    = {180, 180, 180};
    text->m_fontId   = m_fpsFontId;
    text->m_rotation = 0.0f;
    text->m_scaleX   = 1.0f;
    text->m_scaleY   = 1.0f;
    text->m_z        = FPS_Z;

    // Right-align
    TTF_Font* font = getFont(m_fpsFontId);
    int textW = 0, textH = 0;
    if(font)
    {
        TTF_GetStringSize(font, fpsStr.c_str(), 0, &textW, &textH);
    }
    text->m_x = FPS_X - static_cast<float>(textW);
    text->m_y = FPS_Y;

    renderQueueAdd(m_frameId, text);
}
#endif


// ============================================================================
// Public API (free functions delegating to static GameManager instance)
// ============================================================================

static GameManager f_gameManager;


Status initializeGameManager()
{
    return f_gameManager.initialize();
}


void shutdownGameManager()
{
    f_gameManager.shutdown();
}


Status loadGame(GamePtr game, std::function<GamePtr()> restartFactory)
{
    return f_gameManager.loadGame(game, restartFactory);
}


Status unloadGame()
{
    return f_gameManager.unloadGame();
}


Status restartCurrentGame()
{
    return f_gameManager.restartCurrentGame();
}


void openConnectionSettings()
{
    f_gameManager.openSettings();
}


void tickGameManager()
{
    f_gameManager.tick();
}
