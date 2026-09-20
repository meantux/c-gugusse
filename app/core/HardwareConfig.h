#pragma once

#include <optional>
#include <string>
#include <vector>

namespace hqcore {

// One motor's wiring + speed limits, as read from hardwarecfg.json's
// "feeder"/"filmdrive"/"pickup" sections - the exact same file and field
// names as ../GugusseRoller's hardwarecfg.json (pinEnable/pinDirection/
// pinStep/invert/minSpeed/maxSpeed/stopPin/stopState/flags), so an
// existing installation's wiring config just works unmodified.
struct MotorConfig {
	std::string name;
	int pinEnable = -1;
	int pinDirection = -1;
	int pinStep = -1;
	bool invert = false;
	double minSpeed = 1.0;  // steps/sec
	double maxSpeed = 1.0;  // steps/sec

	// The "arm"/"hole" detector sensor: stopPin reads stopState when
	// triggered. pullUp/pullDown mirror hardwarecfg.json's "flags"
	// array ("pullUp"/"pullDown") - the sensor's wiring needs the
	// matching internal bias enabled to read a stable level.
	int stopPin = -1;
	int stopState = 1;
	bool pullUp = false;
	bool pullDown = false;
};

// One motor's speed profile for a given film format, as read from
// hardwarecfg.json's "filmFormats.<name>.<motor>" sections (same field
// names as ../GugusseRoller: speed/speed2/targetTime/ignoreInitial/
// faultTreshold). Deliberately excludes ../GugusseRoller's dynamic speed
// recalibration (calculateNewSpeed()/histo) - speed and speed2 are used
// as fixed values here; that's explicitly deferred, not lost.
struct FilmFormatMotorConfig {
	double speed = 1.0;
	double speed2 = 1.0;
	double targetTime = 1.0; // seconds - turntable move budget
	long ignoreInitial = 1;  // filmdrive: fixed advance step count
	long faultTreshold = 1;  // safety cap: abort if sensor never triggers
};

struct FilmFormatConfig {
	FilmFormatMotorConfig feeder;
	FilmFormatMotorConfig filmdrive;
	FilmFormatMotorConfig pickup;
};

struct HardwareConfig {
	MotorConfig feeder;
	MotorConfig filmdrive;
	MotorConfig pickup;

	// Loads and validates all three motor sections from `path`. Returns
	// std::nullopt if the file is missing, malformed, or any motor
	// section is missing a required pin/speed field - unlike camera
	// settings, there's no safe made-up default for GPIO pin numbers, so
	// callers should treat a nullopt as "motor controls unavailable"
	// rather than falling back to guessed wiring.
	static std::optional<HardwareConfig> load(const std::string &path);

	// Loads the named film format's per-motor speed profile (e.g. "35mm")
	// from the same file's "filmFormats" section. std::nullopt if the
	// format name or any required field is missing.
	static std::optional<FilmFormatConfig> loadFilmFormat(const std::string &path,
								const std::string &formatName);

	// Lists the names of every film format defined in `path`'s
	// "filmFormats" object (e.g. to populate a format-choice dropdown) -
	// does NOT validate that each one's fields are complete/valid the
	// way loadFilmFormat() does for a single name; a name returned here
	// could still fail loadFilmFormat() if hand-edited into invalidity.
	// std::nullopt if the file is missing/malformed or has no
	// "filmFormats" object at all; an empty (but non-nullopt) vector
	// means the object exists but defines no formats.
	static std::optional<std::vector<std::string>>
	listFilmFormatNames(const std::string &path);

	// Writes just the three motors' "invert" flags back to `path`
	// (read-modify-write - the rest of the file, including key order, is
	// preserved). Returns false on failure, including an unparseable
	// existing file (left untouched) or a missing motor section.
	static bool saveMotorInverts(const std::string &path, bool feeder, bool filmdrive,
				      bool pickup);
};

} // namespace hqcore
