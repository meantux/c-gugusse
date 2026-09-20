#pragma once

#include <cstdint>
#include <string>

namespace hqcore {

// Persisted camera settings for the Raspberry Pi HQ camera: the single
// exposure time, the white-balance gains and the regular image controls.
// Saved to hq-camera-settings.json, deliberately SEPARATE from
// hardwarecfg.json (GPIO pin wiring, motor limits - hardware-installation
// config, not camera-specific) matching ../GugusseRoller's own split
// between hardwarecfg.json and its camera/capture settings file. Grouped
// under a top-level "camera" JSON object so a future multi-camera version
// has room to add sibling keys without reshaping these fields.
struct CameraSettings {
	int32_t exposureMicroseconds = 20000;
	float redGain = 1.0f;
	float blueGain = 1.0f;
	float brightness = 0.0f;
	float contrast = 1.0f;
	float saturation = 1.0f;

	// Loads from `path`. Returns default-constructed settings (as above)
	// if the file doesn't exist or fails to parse - never throws, so a
	// missing or hand-edited-into-invalidity file can't crash the app.
	static CameraSettings load(const std::string &path);

	// Writes atomically (write to `path`+".tmp", then rename over
	// `path`) so a crash or power loss mid-write can't corrupt the
	// settings file. Returns false on failure.
	bool save(const std::string &path) const;

	bool operator==(const CameraSettings &) const = default;
};

} // namespace hqcore
