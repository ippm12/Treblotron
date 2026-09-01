/**
 * dart_tuning.hpp
 *
 * The detection knobs a user is allowed to turn, in one place.
 *
 * These live in detect/ rather than next to the settings screen because all
 * four consumers need the same definition and none of them may depend on the
 * others: the game's settings UI edits them, vision/ persists them, net/ puts
 * them on the wire, and the server applies them. One struct means adding a
 * knob is one edit, not five that can drift.
 *
 * Every value here is safe to change while inference is running. That is a
 * deliberate constraint on what belongs in this file: anything that decides
 * which models get loaded, or how a tensor is shaped, is a build-time concern
 * and stays in DartDetectorConfig.
 */

#ifndef DETECT_DART_TUNING_HPP
#define DETECT_DART_TUNING_HPP

#include <string>


/** The state-machine thresholds. Read by DartDetector on every cycle. */
struct DartTuning
{
    /**
     * Minimum consecutive detections before a candidate can be confirmed.
     *
     * A sample count, not a duration: this is about the peak being reproducible
     * across independent inferences. Paired with confirmHoldMs below, which
     * supplies the timing half.
     */
    int confirmFrames = 3;

    /**
     * How long a candidate must hold the same position before it counts.
     *
     * This is what separates a landed dart from one still in flight. In
     * canonical space a dart's apparent motion slows sharply just before
     * impact — the board-plane homography compresses movement as it nears the
     * plane — so a handful of consecutive detections is easy to collect
     * mid-flight, a little off the final resting place. The dart then lands
     * more than DART_MATCH_RADIUS_PX away, a fresh candidate forms, and the
     * same throw is counted twice.
     *
     * A duration fixes that by physics rather than tuning: a dart in flight
     * cannot hold one position for 300 ms, and a landed one does so trivially.
     */
    int confirmHoldMs = 300;

    /**
     * How long the board must be continuously quiet — no hand, no heatmap peak
     * — before the board counts as clear and the turn can end.
     *
     * A duration rather than a cycle count on purpose. These used to be frame
     * counts tuned at ~30 Hz, but the cycle rate is a property of the backend:
     * ~4 Hz on CPU, 30-55 Hz on a GPU. That made the real timing swing by 8x
     * with no code change, and on the fast path the board was declared clear
     * about a third of a second after a hand was last seen — while it was
     * often still withdrawing.
     */
    int clearHoldMs = 1000;

    /** How long a hand must be continuously present before entering Removing. */
    int handEnterMs = 100;
};


/**
 * Everything the settings screen owns: the detector's thresholds plus the
 * capture switch, which is not a detector concern but travels with them —
 * same screen, same file on disk, same message on the wire.
 */
struct DartVisionSettings
{
    DartTuning tuning;

    /**
     * Save the frames behind every confirmed dart.
     *
     * A diagnostic mode, off by default: it is the only way to see what the
     * detector was actually looking at when it scored, including the frame a
     * double count was made on, which is otherwise gone by the time anyone
     * notices the score was wrong. Writes to the machine running inference.
     */
    bool captureOnDetect = false;
};


// ============================================================================
// Bounds
// ============================================================================
//
// Advertised here because three separate places need the same numbers: the
// settings screen clamps the cursor to them, the loader rejects a hand-edited
// file that falls outside them, and the server applies them to whatever a
// client sends rather than trusting it.

constexpr int DART_CONFIRM_FRAMES_MIN = 1;
constexpr int DART_CONFIRM_FRAMES_MAX = 10;
constexpr int DART_CONFIRM_HOLD_MIN   = 0;
constexpr int DART_CONFIRM_HOLD_MAX   = 2000;
constexpr int DART_CLEAR_HOLD_MIN     = 200;
constexpr int DART_CLEAR_HOLD_MAX     = 5000;
constexpr int DART_HAND_ENTER_MIN     = 0;
constexpr int DART_HAND_ENTER_MAX     = 2000;

/**
 * Force every field into range.
 *
 * Applied on load, on edit, and on receipt over the network. A zero
 * confirmFrames would confirm a dart from a single noisy peak and a huge
 * clearHoldMs would hang the turn forever, so nothing downstream should have
 * to wonder whether it was handed a sane value.
 */
void clampDartTuning(DartTuning& tuning);

/** One line for a log, e.g. "confirm=3/300ms clear=1000ms hand=100ms capture=off". */
std::string describeVisionSettings(const DartVisionSettings& settings);

#endif // DETECT_DART_TUNING_HPP
