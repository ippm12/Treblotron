/**
 * vision_settings.hpp
 *
 * The user's detection settings: stored, persisted, and versioned.
 *
 * Companion to vision_link.hpp, which owns the other half of the same settings
 * screen — the server address and how the link is doing. Split because they are
 * genuinely different things: one is about reaching a machine, this is about
 * what that machine does once reached. The screen shows both.
 *
 * Distribution is pull, not push. Nothing here reaches into the active vision
 * source; instead the source polls getVisionSettingsGeneration() and picks up a
 * change when it notices one. That keeps the UI thread out of the inference
 * path entirely, and means a network client that reconnects re-sends the
 * current settings without anyone having to remember to trigger it.
 */

#ifndef VISION_SETTINGS_HPP
#define VISION_SETTINGS_HPP

#include "common_inc.hpp"
#include "detect/dart_tuning.hpp"

#include <cstdint>
#include <string>


/** The current settings. Safe to call from any thread. */
DartVisionSettings getVisionSettings();

/**
 * Bumped on every successful setVisionSettings().
 *
 * A vision source compares this against the value it last acted on to decide
 * whether anything needs doing, which costs one relaxed atomic load per cycle
 * rather than a comparison of the whole struct.
 */
uint32_t getVisionSettingsGeneration();

/**
 * Replace and persist the settings. Values are clamped first.
 *
 * Takes effect within a cycle or two — the local detector picks it up on its
 * next inference pass, and a network client sends it to the server on its next
 * loop. Returns an error only when the file could not be written; the in-memory
 * value is updated either way, so a read-only disk costs persistence but not
 * the edit.
 */
Status setVisionSettings(const DartVisionSettings& settings);

/**
 * Load from disk. A missing file is not an error — it means "defaults", which
 * is the correct state for a fresh install. Unknown keys are ignored and
 * missing keys keep their default, so a file written by an older or newer build
 * still loads.
 */
Status loadVisionSettings(const std::string& path = "./config/vision.txt");

#endif // VISION_SETTINGS_HPP
