#pragma once

#include "dart/dart_detection.hpp"
#include <mutex>
#include <queue>
#include <utility>
#include <vector>

// Keep confirmed throws and collections in inference order. If the UI
// falls behind, let the game score each result before the next handoff.
class DetectionQueue
{
public:
    struct Frame
    {
        std::vector<DartDetection> darts;
        bool collected = false;
    };

    void publish(const std::vector<DartDetection>& darts, bool collected)
    {
        if(darts.empty() && !collected) return;
        std::lock_guard<std::mutex> lock(m_mutex);
        if(!darts.empty()) m_pending.push({darts, false});
        // Positions are consumed in game update(). Even if both arrive in one
        // packet, give the game a tick to score them before ending that turn.
        if(collected) m_pending.push({{}, true});
    }

    void reset()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_pending = {};
    }

    Frame take()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if(m_pending.empty()) return {};
        Frame next = std::move(m_pending.front());
        m_pending.pop();
        return next;
    }

private:
    std::mutex m_mutex;
    std::queue<Frame> m_pending;
};
