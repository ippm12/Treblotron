/**
 * vision_source.hpp
 *
 * Abstract interface for dart detection vision sources. Both the
 * simulator and future camera implementations derive from this.
 */

#ifndef VISION_SOURCE_HPP
#define VISION_SOURCE_HPP

#include "common_inc.hpp"
#include "game_lib/game.hpp"
#include <memory>

class VisionSource
{
    public:
        virtual ~VisionSource() = default;

        /** Initialize the vision source. Called after the frame module is ready. */
        virtual Status init() = 0;

        /** Called once per game tick with delta time in seconds. */
        virtual void tick(float deltaTime) = 0;

        /** Shut down the vision source and release resources. */
        virtual void shutdown() = 0;

        /** Returns true if no darts are on the board. */
        virtual bool isBoardClear() const = 0;

        /** Set the game that receives dart event callbacks. Pass nullptr to disconnect. */
        void setGame(GamePtr game) { m_game = game; }

    protected:
        GamePtr m_game;
};

typedef std::shared_ptr<VisionSource> VisionSourcePtr;

#endif // VISION_SOURCE_HPP
