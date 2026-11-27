#include "../collection_state.hpp"
#include <cassert>
#include <iostream>

constexpr bool testCollection() {
    using State = DartCollectionState;
    using Action = State::Action;
    using Hand = State::Hand;
    State state;
    const State::Clock::time_point start{};
    auto tick = [&](int ms, bool hand, bool peak) {
        return state.update(hand ? Hand::Observed : Hand::Absent, peak,
                            start + std::chrono::milliseconds(ms), 100, 1000);
    };
    // Intermittent candidates cannot create gameplay collection edges.
    for(int i=0;i<100;++i) {
        assert(tick(i*10,false,i%2==0)==Action::TrackDarts);
        assert(state.boardClear);
    }
    state.confirmDart();
    for(int i=100;i<200;++i) {
        assert(tick(i*10,false,i%2==0)==Action::TrackDarts);
        assert(!state.boardClear);
    }
    // No dart tracking even during hand-entry debounce. A brief hand does
    // not discard confirmed darts or turn into a collection.
    assert(tick(2000,true,true)==Action::SuppressDarts);
    assert(tick(2050,false,false)==Action::TrackDarts);
    assert(!state.boardClear && state.mode==State::Mode::Detecting);
    assert(tick(2100,true,true)==Action::SuppressDarts);
    assert(tick(2200,true,true)==Action::StartedRemoving);
    assert(tick(5000,true,false)==Action::SuppressDarts);
    assert(!state.boardClear);
    // A leftover dart keeps removal active after the hand leaves, and
    // resets the full quiet timer even if it appears near the deadline.
    assert(tick(5100,false,true)==Action::SuppressDarts);
    assert(tick(6099,false,false)==Action::SuppressDarts);
    assert(tick(6100,false,true)==Action::SuppressDarts);
    assert(tick(7099,false,false)==Action::SuppressDarts);
    assert(!state.boardClear);
    assert(tick(7100,false,false)==Action::SuppressDarts);
    assert(tick(8098,false,false)==Action::SuppressDarts);
    assert(tick(8099,false,false)==Action::FinishedRemoving);
    assert(state.boardClear);
    // One round-robin observation held in the camera cache is not a gesture.
    assert(tick(8200,true,false)==Action::SuppressDarts);
    assert(state.update(Hand::Cached, false, start + std::chrono::milliseconds(8450),
                        100, 1000)==Action::SuppressDarts);
    assert(tick(8500,false,false)==Action::TrackDarts);
    assert(state.boardClear);
    // Deliberate empty-board gesture after bounce-outs. A long inference gap
    // before the first quiet sample must not use up any of the quiet delay.
    assert(tick(9000,true,false)==Action::SuppressDarts);
    assert(tick(9099,true,false)==Action::SuppressDarts);
    assert(tick(9100,true,false)==Action::StartedRemoving);
    assert(!state.boardClear);
    assert(tick(12000,false,false)==Action::SuppressDarts);
    assert(tick(12999,true,false)==Action::SuppressDarts);
    assert(tick(13000,false,false)==Action::SuppressDarts);
    assert(tick(13999,false,false)==Action::SuppressDarts);
    assert(tick(14000,false,false)==Action::FinishedRemoving);
    assert(tick(15000,false,false)==Action::TrackDarts);
    assert(state.boardClear);
    return true;
}

int main() {
    static_assert(testCollection());
    // Two trips to the board. The leftover dart is visible for three full
    // hold periods between trips; none of those frames may end the turn.
    DartCollectionState collection;
    using Hand = DartCollectionState::Hand;
    using Action = DartCollectionState::Action;
    auto observe = [&](int ms, Hand hand, bool dart) {
        return collection.update(hand, dart, DartCollectionState::Clock::time_point{} +
                                 std::chrono::milliseconds(ms), 100, 1000);
    };
    collection.confirmDart();
    assert(observe(0, Hand::Observed, true) == Action::SuppressDarts);
    assert(observe(100, Hand::Observed, true) == Action::StartedRemoving);
    for(int ms = 200; ms <= 3200; ms += 100)
        assert(observe(ms, Hand::Absent, true) == Action::SuppressDarts);
    assert(observe(3300, Hand::Absent, false) == Action::SuppressDarts);
    assert(observe(4250, Hand::Observed, false) == Action::SuppressDarts);
    assert(observe(4350, Hand::Absent, false) == Action::SuppressDarts);
    collection.interruptObservation(); // Unusable inference during final clearance.
    assert(observe(5500, Hand::Absent, false) == Action::SuppressDarts);
    assert(observe(6499, Hand::Absent, false) == Action::SuppressDarts);
    assert(observe(6500, Hand::Absent, false) == Action::FinishedRemoving);
    assert(observe(8000, Hand::Absent, false) == Action::TrackDarts);
    std::cout << "PASS detect/collection: flicker, hand debounce, remaining darts, full quiet delay\n";
}
