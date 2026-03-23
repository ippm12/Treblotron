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
#include "common_inc.hpp"
#include "frame/frame.hpp"
#include "frame/render_queue.hpp"
#include "game_lib/game_manager.hpp"
#include "game_lib/components/render_shape.hpp"
#include "game_lib/components/render_text.hpp"
#include "games/main_menu.hpp"
#include "game_manager_class.hpp"
#include "vision/vision.hpp"
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


// ============================================================================
// GameManager class
// ============================================================================

static constexpr size_t WINDOW_WIDTH  = 1280;
static constexpr size_t WINDOW_HEIGHT = 720;


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

    // Load the status bar font
    m_barFontId = loadFont("assets/fonts/Roboto-Regular.ttf", 42.0f);
    if(m_barFontId == INVALID_FONT_ID)
    {
        LOG_ERROR(GAME_MANAGER_LOG_ID, "Failed to load bar font");
        deleteFrame(m_frameId);
        m_frameId = INVALID_FRAME_ID;
        return STATUS_ERROR_GENERIC;
    }

    // Load the pause menu font
    m_pauseFontId = loadFont("assets/fonts/Roboto-Regular.ttf", 28.0f);
    if(m_pauseFontId == INVALID_FONT_ID)
    {
        LOG_WARNING(GAME_MANAGER_LOG_ID, "Failed to load pause menu font");
    }

    // Register input handlers — GameManager owns these and forwards to games
    registerFrameKeyHandler(m_frameId, [this](FrameID, uint32_t keycode, bool pressed) {
        if(!pressed || !m_currentGame) return;

        if(m_paused)
        {
            handlePauseKey(keycode);
            return;
        }

        if(keycode == SDLK_ESCAPE && m_currentGame->isPauseable())
        {
            m_paused = true;
            m_pauseCursor = 0;
            return;
        }

        m_currentGame->onKeyDown(keycode);
    });

    registerFrameGamepadButtonHandler(m_frameId, [this](FrameID, uint8_t button, bool pressed) {
        if(!pressed || !m_currentGame) return;

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
            return;
        }

        m_currentGame->onGamepadButton(button, pressed);
    });

    registerFrameTextHandler(m_frameId, [this](FrameID, const char* text) {
        if(m_paused || !m_currentGame) return;
        m_currentGame->onTextInput(text);
    });

    // Load input hint icons for the pause instruction
    m_inputHints.init();

    m_currentGame = nullptr;
    m_lastTickNs = SDL_GetTicksNS();

#ifdef DARTLENS_SHOW_FPS
    m_fpsFontId = loadFont("assets/fonts/Roboto-Regular.ttf", 16.0f);
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

        // Connect vision source to the new game
        setVisionGame(m_currentGame);
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

        // Disconnect vision source from the game
        setVisionGame(nullptr);

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
        if(!m_paused)
        {
            m_currentGame->update(deltaTime);
        }

        renderQueueClearFrame(m_frameId, 40, 40, 40);
        m_currentGame->render();

        if(m_paused)
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

void GameManager::handlePauseKey(uint32_t keycode)
{
    // Determine how many options to show (hide Restart if no factory)
    bool hasRestart = (m_gameFactory != nullptr);
    uint8_t optionCount = hasRestart ? 3 : 2;

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
            // Map cursor index to action depending on whether Restart is available
            // With restart:    0=Resume, 1=Restart, 2=Main Menu
            // Without restart: 0=Resume, 1=Main Menu
            uint8_t action = m_pauseCursor;
            if(!hasRestart && action >= 1)
            {
                action++; // skip restart action
            }

            if(action == 0)
            {
                m_paused = false;
            }
            else if(action == 1)
            {
                m_paused = false;
                restartCurrentGame();
            }
            else
            {
                m_paused = false;
                loadGame(std::make_shared<MainMenu>());
            }
            break;
        }
        case SDLK_ESCAPE:
            m_paused = false;
            break;
        default:
            break;
    }
}


static constexpr uint32_t PAUSE_OVERLAY_Z = 500;

void GameManager::renderPauseMenu()
{
    // Dark overlay
    auto overlay = std::make_shared<RenderShape>();
    overlay->m_type   = ShapeType::Box;
    overlay->m_color  = {20, 20, 20};
    overlay->m_x      = 0.0f;
    overlay->m_y      = 0.0f;
    overlay->m_z      = PAUSE_OVERLAY_Z;
    overlay->m_width  = 1280.0f;
    overlay->m_height = 720.0f;
    renderQueueAdd(m_frameId, overlay);

    // Center panel
    bool hasRestart = (m_gameFactory != nullptr);
    uint8_t optionCount = hasRestart ? 3 : 2;
    float panelW = 400.0f;
    float rowH   = 50.0f;
    float panelH = 80.0f + optionCount * rowH + 20.0f;
    float panelX = (1280.0f - panelW) * 0.5f;
    float panelY = (720.0f - panelH) * 0.5f;

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
    titleText->m_y        = panelY + 20.0f;
    titleText->m_z        = PAUSE_OVERLAY_Z + 2;
    renderQueueAdd(m_frameId, titleText);

    // Menu options
    const char* allOptions[] = {"Resume", "Restart", "Main Menu"};
    // Build visible option list
    const char* visibleOptions[3];
    int vi = 0;
    visibleOptions[vi++] = allOptions[0]; // Resume always
    if(hasRestart)
    {
        visibleOptions[vi++] = allOptions[1]; // Restart
    }
    visibleOptions[vi++] = allOptions[2]; // Main Menu

    float optRowW = 260.0f;
    float optStartY = panelY + 80.0f;
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
            bg->m_height = rowH - 6.0f;
            renderQueueAdd(m_frameId, bg);
        }

        int optW = 0, optH = 0;
        if(optFont)
        {
            TTF_GetStringSize(optFont, visibleOptions[i], 0, &optW, &optH);
        }

        auto optText = std::make_shared<RenderText>();
        optText->m_text     = visibleOptions[i];
        optText->m_color    = isSelected ? Color{255, 255, 255} : Color{160, 160, 170};
        optText->m_fontId   = optFontId;
        optText->m_rotation = 0.0f;
        optText->m_scaleX   = 1.0f;
        optText->m_scaleY   = 1.0f;
        optText->m_x        = rowX + (optRowW - optW) * 0.5f;
        optText->m_y        = rowY + (rowH - 6.0f - optH) * 0.5f;
        optText->m_z        = PAUSE_OVERLAY_Z + 3;
        renderQueueAdd(m_frameId, optText);
    }
}


// ============================================================================
// Status bar rendering
// ============================================================================

static constexpr float    BAR_Y       = 620.0f;
static constexpr float    BAR_HEIGHT  = 100.0f;
static constexpr float    BAR_WIDTH   = 1280.0f;
static constexpr uint32_t BAR_Z       = UINT32_MAX - 10;
static constexpr float    BAR_TEXT_Y  = BAR_Y + 30.0f;
static constexpr float    BAR_PAD_X   = 20.0f;
static constexpr float    DART_CIRCLE_DIAMETER = 35.0f;
static constexpr float    DART_CIRCLE_SPACING  = 52.0f;
static constexpr float    DART_CIRCLE_GAP      = 40.0f;  // Gap between name text and first dart circle
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
            status->m_x        = BAR_WIDTH - BAR_PAD_X - info.statusText.length() * 22.0f;
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
            status->m_x        = BAR_WIDTH - BAR_PAD_X - info.statusText.length() * 22.0f;
            status->m_y        = BAR_TEXT_Y;
            status->m_z        = BAR_Z + 1;
            renderQueueAdd(m_frameId, status);
        }
    }
    // Blank: background only

    // Pause hint (shown for all pauseable games, regardless of state)
    if(m_currentGame && m_currentGame->isPauseable() && m_pauseFontId != INVALID_FONT_ID)
    {
        m_inputHints.render(m_frameId, m_pauseFontId, BAR_WIDTH - 150.0f,
                            BAR_Y - 40.0f, BAR_Z, {
            {SDLK_ESCAPE, SDL_GAMEPAD_BUTTON_START, "pause"}
        });
    }
}


#ifdef DARTLENS_SHOW_FPS
static constexpr float    FPS_UPDATE_INTERVAL = 0.5f;
static constexpr float    FPS_X               = 1220.0f;
static constexpr float    FPS_Y               = 4.0f;
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


void tickGameManager()
{
    f_gameManager.tick();
}
