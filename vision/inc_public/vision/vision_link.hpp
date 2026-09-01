/**
 * vision_link.hpp
 *
 * The address of the remote inference server, and how the link to it is doing.
 *
 * Kept in the vision module rather than in game_lib because the address is a
 * vision concern and the dependency only runs one way: the settings UI lives in
 * game_lib and calls down into vision, never the reverse.
 *
 * Both halves are meaningful even in builds that do their own inference — the
 * address is simply unused and the link reports NotApplicable, so UI code needs
 * no build-time branching.
 */

#ifndef VISION_LINK_HPP
#define VISION_LINK_HPP

#include "common_inc.hpp"
#include <string>


// ============================================================================
// Server address
// ============================================================================

/**
 * Current server address as "host" or "host:port". Empty when nothing has been
 * configured yet, which the UI surfaces as "no server configured" rather than
 * silently trying to reach localhost.
 */
std::string getInferenceServerAddress();

/**
 * Set and persist the address. Takes effect on the next reconnect attempt —
 * a few seconds — so there is no need to restart after editing it.
 *
 * Writes through to disk immediately; an address the user typed once should
 * survive a power cut on the way to the next launch.
 */
Status setInferenceServerAddress(const std::string& hostPort);

/**
 * Load the saved address. Falls back to the TREBLOTRON_SERVER environment
 * variable when no file exists, so an existing scripted deployment keeps
 * working; once anything is saved through the UI, the file wins.
 */
Status loadInferenceServerAddress(const std::string& path = appDataPath("config/server.txt"));


// ============================================================================
// Link health
// ============================================================================

enum class VisionLinkState : uint8_t
{
    NotApplicable,  ///< This build runs its own inference; there is no link.
    Disconnected,   ///< No server, wrong address, or it went away.
    Degraded,       ///< Connected but slow, or not scoring frames right now.
    Healthy,        ///< Connected and keeping up.
};

/** Current link health. Safe to call every frame. */
VisionLinkState getVisionLinkState();

/**
 * One line of human detail for the settings page and the warning banner,
 * e.g. "192.168.1.50:9876 - 31 ms" or "no server configured".
 */
std::string getVisionLinkDetail();

#endif // VISION_LINK_HPP
