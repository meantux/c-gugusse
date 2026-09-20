#include "ExportSettings.h"

#include "JsonFile.h"

namespace hqcore {

ExportSettings ExportSettings::load(const std::string &path) {
	ExportSettings settings;

	nlohmann::ordered_json root;
	if (!readJsonObject(path, root))
		return settings;

	try {
		if (root.value("saveMode", "ftp") == "local")
			settings.mode = Mode::Local;
		settings.localPath = root.value("localFilePath", settings.localPath);
	} catch (const nlohmann::json::exception &) {
		return ExportSettings{};
	}
	return settings;
}

bool ExportSettings::save(const std::string &path) const {
	nlohmann::ordered_json root;
	if (!readJsonObject(path, root))
		return false;
	root["saveMode"] = mode == Mode::Local ? "local" : "ftp";
	root["localFilePath"] = localPath;
	return writeJsonAtomic(path, root);
}

} // namespace hqcore
