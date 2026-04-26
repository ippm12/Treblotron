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
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

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

        /** Clear all tracked darts and reset the board state. */
        virtual void resetDarts() = 0;

        /**
         * Returns true once if the user has explicitly asked the board to
         * be reset since the last call (e.g. clicked the sim's Collect
         * Darts button). Distinct from isBoardClear() — fires even when
         * the board was already clear. Default: never. Sources that don't
         * have an explicit reset signal (real cameras) leave it as the
         * default; the manager's isBoardClear() transition still catches
         * the natural hand-clear case for them.
         */
        virtual bool consumeBoardResetRequest() { return false; }

        /**
         * Copy the latest inference heatmap (row-major, values in [0, 1]) out
         * of the vision source. Returns false if the source doesn't produce a
         * heatmap (e.g. the sim) or no inference has run yet.
         */
        virtual bool getLatestHeatmap(std::vector<float>& out,
                                      uint32_t& width, uint32_t& height) const
        {
            (void)out; (void)width; (void)height;
            return false;
        }

        /**
         * True if the source is still bringing itself up (e.g. the
         * TensorRT source is building / loading its FP16 engine). When
         * true, the main loop should render a loading screen instead of
         * the normal UI.
         *
         * Default: false — sources that come up synchronously never
         * report an "initializing" state.
         */
        virtual bool isInitializing() const { return false; }

        /**
         * True if the source's init failed permanently. The loading
         * screen should stay visible in this state and render the error
         * (from getInitStatus) until the user closes the window.
         *
         * Default: false — sources that come up synchronously can't
         * fail.
         */
        virtual bool isFailed() const { return false; }

        /**
         * Fractional init progress in [0, 1]. Only meaningful while
         * isInitializing() is true. Monotonic — never moves backward.
         * Default: 1.0 (ready).
         */
        virtual float getInitProgress() const { return 1.0f; }

        /**
         * Raw "still doing something" iteration counter. Increments while
         * a long-running internal step is grinding away (e.g. TRT
         * timing tactics). Loading screens display it next to the
         * progress bar so the user can tell the program isn't hung.
         *
         * Default: 0 — sources that come up instantly have no
         * iterations to report.
         */
        virtual uint64_t getInitIteration() const { return 0; }

        /**
         * Short human-readable status for the loading screen
         * (e.g. "Timing CUDA tactics"). May change mid-init as
         * sub-phases start.
         */
        virtual std::string getInitStatus() const { return {}; }

        /**
         * Short human-readable runtime status describing the current
         * detection state — e.g. "Detecting", "Detecting (entering 1/2)",
         * "Removing (clear 5/10)". Sources that don't have a state
         * machine return an empty string and the UI hides the line.
         */
        virtual std::string getDetectionStatus() const { return {}; }

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
