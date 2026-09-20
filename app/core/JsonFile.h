#pragma once

#include <nlohmann/json.hpp>

#include <cstdio>
#include <fstream>
#include <string>

namespace hqcore {

// Read-modify-write helpers for the shared config files (hardwarecfg.json,
// ftp.json). They use ordered_json so a rewrite keeps the file's existing
// key order and doesn't touch keys this app doesn't know about (the same
// files are shared with ../GugusseRoller).

// Loads `path` as a JSON object. A missing file yields an empty object
// (so a first save can create it); a file that exists but isn't a valid
// JSON object yields false - callers must not overwrite what they can't
// parse, or a hand-edited typo would silently wipe the file.
inline bool readJsonObject(const std::string &path, nlohmann::ordered_json &out) {
	std::ifstream in(path);
	if (!in) {
		out = nlohmann::ordered_json::object();
		return true;
	}
	try {
		in >> out;
	} catch (const nlohmann::json::exception &) {
		return false;
	}
	return out.is_object();
}

// Writes atomically (write `path`+".tmp", then rename over `path`) so a
// crash or power loss mid-write can't corrupt the file. Same 4-space
// indent as ../GugusseRoller's json.dump(indent=4).
inline bool writeJsonAtomic(const std::string &path, const nlohmann::ordered_json &json) {
	const std::string tmpPath = path + ".tmp";
	{
		std::ofstream out(tmpPath);
		if (!out)
			return false;
		out << json.dump(4) << '\n';
		if (!out)
			return false;
	}
	return std::rename(tmpPath.c_str(), path.c_str()) == 0;
}

} // namespace hqcore
