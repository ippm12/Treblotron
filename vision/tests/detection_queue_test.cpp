#include "../vision_source.hpp"
#include <cassert>
#include <iostream>

class TestCamera : public CameraVisionSource
{
public:
    Status init() override { return STATUS_OK; }
    void shutdown() override {}
    void resetDarts() override { m_detections.reset(); }
    using CameraVisionSource::m_detections;
};

int main()
{
    TestCamera camera;
    std::vector<int> positions;
    int pickups = 0;
    camera.setCallbacks({}, [&](float angle, float) { positions.push_back(static_cast<int>(angle)); },
                       [&] { ++pickups; });
    // Even when inference outpaces the UI, the outgoing player gets the
    // final throw, then one collection, then the incoming player's throw.
    camera.m_detections.publish({{30, 0.5f}}, true);
    camera.m_detections.publish({}, false);
    camera.m_detections.publish({{90, 0.3f}}, false);
    camera.tick(0);
    assert(positions == std::vector<int>{30} && pickups == 0);
    camera.tick(0);
    assert(positions.size() == 1 && pickups == 1);
    camera.tick(0);
    assert(positions.size() == 2 && positions.back() == 90 && pickups == 1);
    for(int i=0; i<120; ++i) camera.tick(0);
    assert(pickups == 1);
    camera.m_detections.publish({}, true);  // Gesture with no confirmed dart.
    camera.tick(0);
    assert(pickups == 2);
    camera.m_detections.publish({}, true);
    camera.resetDarts();
    camera.tick(0);
    assert(pickups == 2);
    std::cout << "PASS vision/detection_queue\n";
}
