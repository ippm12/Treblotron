/**
 * game.hpp
 *
 * Abstract base class for all games. Subclass this to create
 * a game implementation (e.g., X01, main menu).
 */

#ifndef GAME_HPP
#define GAME_HPP

#include "common_inc.hpp"
#include "frame/frame.hpp"
#include "flecs.h"
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <queue>
#include <string>

/** State of the game for the status bar. */
enum class GameState
{
    Blank,          // Bar is empty / game is computing
    PlayerTurn,     // A player is actively throwing
    CollectDarts    // Round over, collect darts
};

/** Information for the GameManager status bar. */
struct GameBarInfo
{
    GameState   state          = GameState::Blank;
    std::string playerName;      // PlayerTurn only
    uint8_t     throwsRemaining = 0; // PlayerTurn only (0-3)
    std::string statusText;      // PlayerTurn only — right-side text
};

/** Position data from a dart throw (polar coordinates). */
struct DartPosition
{
    float angle;            // degrees, atan2 convention (0=right, positive CCW)
    float normalizedRadius; // 0.0 (center) to 1.0 (double outer edge)
};

class Game;
typedef std::shared_ptr<Game> GamePtr;

class Game
{
    public:
        Game(const std::string& pName);
        virtual ~Game();

        std::string getName() const;
        FrameID getFrameId() const;

        /**
         * Called once when the game is loaded.
         * The FrameID of the game window is passed in.
         */
        virtual Status init(FrameID frameId) = 0;

        /**
         * Called every frame with delta time in seconds.
         */
        virtual void update(float deltaTime) = 0;

        /**
         * Enqueue renderable objects for this frame.
         * The GameManager handles clear, flush, and present.
         */
        virtual void render() = 0;

        /**
         * Called once when the game is unloaded.
         */
        virtual void shutdown() = 0;

        /** Return current state for the GameManager status bar. Default: Blank. */
        virtual GameBarInfo getBarInfo() const { return {}; }

        /** Maximum number of players this game supports. */
        virtual uint8_t getMaxPlayers() const { return 1; }

        /**
         * Input callbacks — called by the GameManager when input events occur.
         * Override in subclasses to handle keyboard, gamepad, and text input.
         * These are NOT called when the game is paused by the GameManager.
         */
        virtual void onKeyDown(uint32_t keycode) { (void)keycode; }
        virtual void onGamepadButton(uint8_t button, bool pressed) { (void)button; (void)pressed; }
        virtual void onTextInput(const char* text) { (void)text; }

        /** Whether the GameManager should allow pausing this game. Default: true. */
        virtual bool isPauseable() const { return true; }

        /**
         * Vision callbacks — called from the vision source (potentially another thread).
         * Default implementations atomically store data into thread-safe structures
         * that update() consumes on the main thread. Override in subclasses for
         * custom behavior (e.g., games that need immediate processing).
         */
        virtual void onDartLanded();
        virtual void onDartPositionCalculated(float angle, float normalizedRadius);

    protected:
        /**
         * Thread-safe consume helpers for subclasses to call in update().
         * These drain the data stored by the vision callbacks.
         */

        /** Returns how many darts landed since last consume, resets counter to 0. */
        uint32_t consumeDartLandedCount();

        /** Pops one position from the queue. Returns false if empty. */
        bool popDartPosition(DartPosition& out);

        FrameID m_frameId = INVALID_FRAME_ID;
        flecs::world m_world;

    private:
        std::string m_name;

        // Thread-safe vision event storage
        std::atomic<uint32_t>    m_dartLandedCount{0};
        std::mutex               m_dartPositionMutex;
        std::queue<DartPosition> m_dartPositionQueue;
};

#endif // GAME_HPP
