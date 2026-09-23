#pragma once

#include <string>

namespace hqcore {

// Persisted cross-session UI preferences - saved to preferences.json,
// deliberately separate from hardwarecfg.json (the film formats' actual
// speed/limit data - hardware/installation config, not a per-user choice)
// and from hq-camera-settings.json (camera exposure/white balance values,
// an unrelated concern). Currently the selected film format, the
// still-capture file format and the feeder/pickup reel direction; room
// to grow with more preferences later
// without disturbing either of those files.
struct Preferences {
	std::string filmFormat;    // empty if never saved
	std::string captureFormat; // "DNG" or "JPG"; empty if never saved
	std::string reelDirection; // "CCW" or "CW"; empty if never saved

	// Loads from `path`. Returns default-constructed preferences (as
	// above) if the file doesn't exist or fails to parse - never throws,
	// so a missing or hand-edited-into-invalidity file can't crash the
	// app.
	static Preferences load(const std::string &path);

	// Writes atomically (write to `path`+".tmp", then rename over
	// `path`) so a crash or power loss mid-write can't corrupt the
	// preferences file. Returns false on failure.
	bool save(const std::string &path) const;
};

} // namespace hqcore
