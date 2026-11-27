#pragma once
#include <chrono>
#include <optional>

// Only a completed hand-removal cycle emits FinishedRemoving. Keep the
// board level for detector diagnostics and older network clients.
struct DartCollectionState {
    using Clock = std::chrono::steady_clock;
    enum class Hand { Absent, Cached, Observed };
    enum class Mode { Detecting, Removing };
    enum class Action { TrackDarts, SuppressDarts, StartedRemoving, FinishedRemoving };
    Mode mode = Mode::Detecting;
    bool boardClear = true;
    bool handWasPresent = false;
    Clock::time_point handSince{};
    std::optional<Clock::time_point> quietSince;

    constexpr Action update(Hand hand, bool peak, Clock::time_point now,
                            int handEnterMs, int clearHoldMs) {
        if(mode == Mode::Removing) {
            if(hand != Hand::Absent || peak) {
                quietSince.reset();
                return Action::SuppressDarts;
            }
            if(!quietSince) quietSince = now;
            if(now - *quietSince >= std::chrono::milliseconds(clearHoldMs)) {
                mode = Mode::Detecting;
                handWasPresent = false;
                boardClear = true;
                return Action::FinishedRemoving;
            }
            return Action::SuppressDarts;
        }
        if(hand != Hand::Absent) {
            // A previous camera's positive still suppresses tracking while the
            // other views rotate through, but cannot confirm its own debounce.
            if(hand == Hand::Cached) return Action::SuppressDarts;
            if(!handWasPresent) { handWasPresent = true; handSince = now; }
            if(now - handSince >= std::chrono::milliseconds(handEnterMs)) {
                mode = Mode::Removing;
                // A deliberate collection also ends a turn of bounce-outs.
                boardClear = false;
                quietSince.reset();
                return Action::StartedRemoving;
            }
            // Even during hand-entry debounce, never confirm a dart in a hand.
            return Action::SuppressDarts;
        }
        handWasPresent = false;
        return Action::TrackDarts;
    }

    constexpr void confirmDart() { boardClear = false; }

    constexpr void interruptObservation() {
        handWasPresent = false;
        quietSince.reset();
    }
};
