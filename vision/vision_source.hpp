/**
 * vision_source.hpp
 *
 * Abstract interface for dart detection vision sources. Both the
 * simulator and future camera implementations derive from this.
 *
 * INTERNAL HEADER — do not include from outside the vision module.
 * Use vision/vision.hpp for the public API.
 */

#ifndef VISION_SOURCE_HPP
#define VISION_SOURCE_HPP

#include "common_inc.hpp"
#include <functional>
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

        /** Set callbacks for dart events. Pass nullptr to disconnect. */
        void setCallbacks(std::function<void()> onDartLanded,
                          std::function<void(float, float)> onDartPositionCalculated)
        {
            m_onDartLanded = std::move(onDartLanded);
            m_onDartPositionCalculated = std::move(onDartPositionCalculated);
        }

    protected:
        std::function<void()>          m_onDartLanded;
        std::function<void(float, float)> m_onDartPositionCalculated;
};

typedef std::shared_ptr<VisionSource> VisionSourcePtr;

#endif // VISION_SOURCE_HPP
