#pragma once

#include <string>

namespace hqcore {

// Where the app keeps its user configuration and finds its read-only data.
//
//  - Config directory: $XDG_CONFIG_HOME/c-gugusse, else ~/.config/c-gugusse.
//    Holds ftp.json, hardwarecfg.json, preferences.json and
//    hq-camera-settings.json (all written by the app).
//  - Data directory: holds defaults/ (first-run copies of those files) and
//    assets/icons/. Resolved in this order:
//      1. $C_GUGUSSE_DATA_DIR
//      2. the source tree, when running from its build directory
//         (<exe>/../.. contains assets/ and defaults/)
//      3. the install location compiled in (<prefix>/share/c-gugusse)

std::string configDir();
std::string dataDir();

std::string configFile(const std::string &name);
std::string assetFile(const std::string &relativePath);

// Copies each defaults/<name> into the config directory unless a file of
// that name is already there (existing files are never touched). Creates
// the config directory if needed. `error` is empty on success, otherwise
// says what could not be created (missing default, unwritable directory).
struct SeedResult {
	std::string dir;
	std::string error; // empty on success
};
SeedResult seedConfigFiles();

} // namespace hqcore
