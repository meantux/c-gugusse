#include "Preferences.h"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <fstream>

namespace hqcore {

Preferences Preferences::load(const std::string &path) {
	Preferences prefs;

	std::ifstream in(path);
	if (!in)
		return prefs;

	nlohmann::json root;
	try {
		in >> root;
	} catch (const nlohmann::json::exception &) {
		return prefs;
	}
	if (!root.is_object())
		return prefs;

	try {
		prefs.filmFormat = root.value("filmFormat", prefs.filmFormat);
		prefs.captureFormat = root.value("captureFormat", prefs.captureFormat);
	} catch (const nlohmann::json::exception &) {
		return Preferences{};
	}
	return prefs;
}

bool Preferences::save(const std::string &path) const {
	nlohmann::json root;
	root["filmFormat"] = filmFormat;
	root["captureFormat"] = captureFormat;

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
