/**
 * app_paths.hpp
 *
 * Where files live once the program is installed rather than run from a build
 * tree.
 *
 * Everything used to be relative to the current working directory, which is
 * correct for exactly one situation: launching the binary from its own build
 * output. Installed under Program Files, the same paths resolve inside a
 * read-only directory, so the calibration you just spent 120 clicks on fails to
 * save and nothing says why. That failure is silent by construction — the code
 * asked to write a file and was told no, which is indistinguishable from any
 * other IO error and just as easy to ignore.
 *
 * So paths are split by what they are, not by where they happen to sit:
 *
 *   appAssetPath()  read-only things shipped with the program — models, fonts,
 *                   icons. Beside the executable, wherever that is.
 *   appDataPath()   things the program writes — configuration, logs, captures.
 *                   Under the user's own data directory.
 *
 * Lives in debug/ because every module already reaches this header through
 * common_inc.hpp, so it adds no dependency edge to a graph that is deliberately
 * one-directional.
 */

#ifndef DEBUG_APP_PATHS_HPP
#define DEBUG_APP_PATHS_HPP

#include <string>


/**
 * Set the folder name used under the user's data directory.
 *
 * Call once at startup, before anything writes. Defaults to "Dartmatic", which
 * is right for both binaries — the game and the inference server share a
 * calibration and settings, and keeping them in one place is what lets you run
 * the server on the same machine that once ran the game and have it pick up the
 * board you already calibrated.
 */
void setAppDataFolderName(const std::string& name);

/**
 * Absolute path to a writable file or directory, creating parent directories as
 * needed.
 *
 *   %LOCALAPPDATA%\Dartmatic\<relative>          on Windows
 *   $XDG_DATA_HOME/dartmatic/<relative>          on Linux, or
 *   ~/.local/share/dartmatic/<relative>
 *
 * In a portable install (see isPortableInstall) this is just `<relative>`
 * resolved against the executable's own directory.
 */
std::string appDataPath(const std::string& relative);

/**
 * Absolute path to a read-only file shipped alongside the executable.
 *
 * Resolved against the executable's directory rather than the working
 * directory, so a Start Menu shortcut — which launches with the working
 * directory set to somewhere unrelated — still finds the models.
 */
std::string appAssetPath(const std::string& relative);

/**
 * True when a file named `portable.txt` sits beside the executable.
 *
 * Then writable state goes next to the binary instead of into the user profile,
 * which is what a build tree wants and what someone running from a USB stick
 * wants. CMake stages this marker into build directories; the installer does
 * not ship it, so an installed copy always uses the user profile.
 */
bool isPortableInstall();

/** Directory containing the running executable, without a trailing separator. */
std::string executableDirectory();

#endif // DEBUG_APP_PATHS_HPP
