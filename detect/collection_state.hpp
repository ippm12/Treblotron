#pragma once
#include <chrono>

// Gameplay's board-clear signal is latched. A disappearing candidate is not
// a collection; only a hand-removal cycle (or explicit reset) can clear it.
struct DartCollectionState {
    using Clock = std::chrono::steady_clock;
    enum class Mode { Detecting, Removing };
    enum class Action { TrackDarts, SuppressDarts, StartedRemoving, FinishedRemoving };
    Mode mode = Mode::Detecting;
    bool boardClear = true;
    bool handWasPresent = false;
    Clock::time_point handSince{};
    Clock::time_point lastBusy = Clock::now();

    Action update(bool hand, bool peak, Clock::time_point now,
                  int handEnterMs, int clearHoldMs) {
        if(hand || peak) lastBusy = now;
        if(mode == Mode::Removing) {
            if(!hand && !peak &&
               std::chrono::duration_cast<std::chrono::milliseconds>(now-lastBusy).count() >= clearHoldMs) {
                mode = Mode::Detecting;
                handWasPresent = false;
                boardClear = true;
                return Action::FinishedRemoving;
            }
            return Action::SuppressDarts;
        }
        if(hand) {
            if(!handWasPresent) { handWasPresent = true; handSince = now; }
            if(std::chrono::duration_cast<std::chrono::milliseconds>(now-handSince).count() >= handEnterMs) {
                mode = Mode::Removing;
                boardClear = false;
                lastBusy = now;
                return Action::StartedRemoving;
            }
            // Even during hand-entry debounce, never confirm a dart in a hand.
            return Action::SuppressDarts;
        }
        handWasPresent = false;
        return Action::TrackDarts;
    }

    void confirmDart() { boardClear = false; }
};
