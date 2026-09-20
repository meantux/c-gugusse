#include "CameraSettings.h"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <fstream>

namespace hqcore {

CameraSettings CameraSettings::load(const std::string &path) {
	CameraSettings settings;

	std::ifstream in(path);
	if (!in)
		return settings;

	nlohmann::json root;
	try {
		in >> root;
	} catch (const nlohmann::json::exception &) {
		return settings;
	}
	if (!root.is_object())
		return settings;

	try {
		const auto camera = root.value("camera", nlohmann::json::object());
		settings.exposureMicroseconds =
			camera.value("exposureMicroseconds", settings.exposureMicroseconds);
		settings.redGain = camera.value("redGain", settings.redGain);
		settings.blueGain = camera.value("blueGain", settings.blueGain);
		settings.brightness = camera.value("brightness", settings.brightness);
		settings.contrast = camera.value("contrast", settings.contrast);
		settings.saturation = camera.value("saturation", settings.saturation);
	} catch (const nlohmann::json::exception &) {
		// Wrong value types in a hand-edited file - fall back to defaults.
		return CameraSettings{};
	}

	return settings;
}

bool CameraSettings::save(const std::string &path) const {
	nlohmann::json root;
	root["camera"]["exposureMicroseconds"] = exposureMicroseconds;
	root["camera"]["redGain"] = redGain;
	root["camera"]["blueGain"] = blueGain;
	root["camera"]["brightness"] = brightness;
	root["camera"]["contrast"] = contrast;
	root["camera"]["saturation"] = saturation;

	const std::string tmpPath = path + ".tmp";
	{
		std::ofstream out(tmpPath);
		if (!out)
			return false;
		out << root.dump(4) << '\n';
		if (!out)
			return false;
	}

	return std::rename(tmpPath.c_str(), path.c_str()) == 0;
}

} // namespace hqcore
