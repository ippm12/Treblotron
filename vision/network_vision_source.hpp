/**
 * network_vision_source.hpp
 *
 * Dart detection performed by a remote inference server. Captures locally,
 * ships JPEG frames over TCP, and turns the dart events that come back into
 * the same callbacks every other vision source fires — so nothing above the
 * VisionSource interface can tell the difference.
 *
 * This is what lets the game run on a Raspberry Pi that can't afford the
 * models: the Pi keeps the cameras and the UI, and a desktop or Jetson does
 * the inference.
 *
 * The server's model-loading progress is mirrored into isInitializing() /
 * getInitProgress() / getInitStatus(), so the existing loading screen covers
 * the remote engine build and the connection retries with no changes.
 *
 * INTERNAL HEADER — do not include from outside the vision module.
 */

#ifndef NETWORK_VISION_SOURCE_HPP
#define NETWORK_VISION_SOURCE_HPP

#include "vision_source.hpp"
#include "vision/vision.hpp"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>


class NetworkVisionSource : public VisionSource
{
    public:
        NetworkVisionSource();
        ~NetworkVisionSource() override;

        Status init() override;
        void   tick(float deltaTime) override;
        void   shutdown() override;
        bool   isBoardClear() const override;
        void   resetDarts() override;
        bool   getLatestHeatmap(std::vector<float>& out,
                                uint32_t& width, uint32_t& height) const override;

        bool        isInitializing() const override;
        bool        isFailed() const override;
        float       getInitProgress() const override;
        uint64_t    getInitIteration() const override;
        std::string getInitStatus() const override;
        std::string getDetectionStatus() const override;

    private:
        void clientThreadMain();

        /** One connect + handshake + serve cycle. Returns when the link drops. */
        bool runSession();

        std::thread       m_thread;
        std::atomic<bool> m_running{false};
        std::atomic<bool> m_resetRequested{false};
        std::atomic<bool> m_boardClear{true};

        // Mirrors the server's build state so the loading screen can show it.
        // Stays "initializing" while we're still trying to connect, so an
        // unreachable server reads as "not ready yet" rather than a hard
        // failure — the Pi may well boot before the server does.
        std::atomic<bool>     m_connected{false};
        std::atomic<bool>     m_ready{false};
        std::atomic<bool>     m_failed{false};
        std::atomic<float>    m_initProgress{0.0f};
        std::atomic<uint64_t> m_initIteration{0};
        mutable std::mutex    m_statusMutex;
        std::string           m_initStatus = "Connecting to inference server";
        std::string           m_detectionStatus;

        std::mutex                          m_eventMutex;
        std::queue<std::pair<float, float>> m_newDartEvents;

        mutable std::mutex m_heatmapMutex;
        std::vector<float> m_latestHeatmap;
        uint32_t           m_latestHeatmapW = 0;
        uint32_t           m_latestHeatmapH = 0;
};

#endif // NETWORK_VISION_SOURCE_HPP
