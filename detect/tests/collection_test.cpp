#include "../collection_state.hpp"
#include <cassert>
#include <iostream>

int main() {
    using State = DartCollectionState;
    using Action = State::Action;
    State state;
    const auto start = State::Clock::now();
    auto tick = [&](int ms, bool hand, bool peak) {
        return state.update(hand, peak, start + std::chrono::milliseconds(ms), 100, 1000);
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
    assert(tick(7100,false,false)==Action::FinishedRemoving);
    assert(state.boardClear);
    assert(tick(7110,false,false)==Action::TrackDarts);
    assert(tick(7120,false,true)==Action::TrackDarts);
    assert(tick(7130,false,false)==Action::TrackDarts);
    assert(state.boardClear);
    std::cout << "PASS detect/collection: flicker, hand debounce, remaining darts, full quiet delay\n";
}
