#include "HardwareConfig.h"

#include "JsonFile.h"

#include <nlohmann/json.hpp>

#include <fstream>

namespace hqcore {

namespace {

std::optional<MotorConfig> parseMotor(const nlohmann::json &root,
				       const std::string &key) {
	if (!root.contains(key) || !root[key].is_object())
		return std::nullopt;
	const auto &j = root[key];

	MotorConfig cfg;
	cfg.name = key;
	if (!j.contains("pinEnable") || !j.contains("pinDirection") ||
	    !j.contains("pinStep") || !j.contains("minSpeed") ||
	    !j.contains("maxSpeed") || !j.contains("stopPin") ||
	    !j.contains("stopState")) {
		return std::nullopt;
	}

	try {
		cfg.pinEnable = j.at("pinEnable").get<int>();
		cfg.pinDirection = j.at("pinDirection").get<int>();
		cfg.pinStep = j.at("pinStep").get<int>();
		cfg.invert = j.value("invert", false);
		cfg.minSpeed = j.at("minSpeed").get<double>();
		cfg.maxSpeed = j.at("maxSpeed").get<double>();
		cfg.stopPin = j.at("stopPin").get<int>();
		cfg.stopState = j.at("stopState").get<int>();
	} catch (const nlohmann::json::exception &) {
		return std::nullopt;
	}

	if (cfg.minSpeed <= 0.0 || cfg.maxSpeed < cfg.minSpeed)
		return std::nullopt;

	if (j.contains("flags") && j["flags"].is_array()) {
		for (const auto &flag : j["flags"]) {
			if (flag == "pullUp")
				cfg.pullUp = true;
			else if (flag == "pullDown")
				cfg.pullDown = true;
		}
	}

	return cfg;
}

std::optional<FilmFormatMotorConfig> parseFilmFormatMotor(const nlohmann::json &root,
							    const std::string &key) {
	if (!root.contains(key) || !root[key].is_object())
		return std::nullopt;
	const auto &j = root[key];

	if (!j.contains("speed") || !j.contains("speed2") || !j.contains("targetTime") ||
	    !j.contains("ignoreInitial") || !j.contains("faultTreshold")) {
		return std::nullopt;
	}

	FilmFormatMotorConfig cfg;
	try {
		cfg.speed = j.at("speed").get<double>();
		cfg.speed2 = j.at("speed2").get<double>();
		cfg.targetTime = j.at("targetTime").get<double>();
		cfg.ignoreInitial = j.at("ignoreInitial").get<long>();
		cfg.faultTreshold = j.at("faultTreshold").get<long>();
	} catch (const nlohmann::json::exception &) {
		return std::nullopt;
	}

	if (cfg.speed <= 0.0 || cfg.speed2 <= 0.0 || cfg.targetTime <= 0.0 ||
	    cfg.ignoreInitial <= 0 || cfg.faultTreshold <= 0) {
		return std::nullopt;
	}

	return cfg;
}

} // namespace

std::optional<HardwareConfig> HardwareConfig::load(const std::string &path) {
	std::ifstream in(path);
	if (!in)
		return std::nullopt;

	nlohmann::json root;
	try {
		in >> root;
	} catch (const nlohmann::json::exception &) {
		return std::nullopt;
	}
	if (!root.is_object())
		return std::nullopt;

	auto feeder = parseMotor(root, "feeder");
	auto filmdrive = parseMotor(root, "filmdrive");
	auto pickup = parseMotor(root, "pickup");
	if (!feeder || !filmdrive || !pickup)
		return std::nullopt;

	HardwareConfig cfg;
	cfg.feeder = std::move(*feeder);
	cfg.filmdrive = std::move(*filmdrive);
	cfg.pickup = std::move(*pickup);
	return cfg;
}

std::optional<FilmFormatConfig> HardwareConfig::loadFilmFormat(const std::string &path,
								 const std::string &formatName) {
	std::ifstream in(path);
	if (!in)
		return std::nullopt;

	nlohmann::json root;
	try {
		in >> root;
	} catch (const nlohmann::json::exception &) {
		return std::nullopt;
	}
	if (!root.is_object() || !root.contains("filmFormats") ||
	    !root["filmFormats"].is_object()) {
		return std::nullopt;
	}
	const auto &formats = root["filmFormats"];
	if (!formats.contains(formatName) || !formats[formatName].is_object())
		return std::nullopt;
	const auto &format = formats[formatName];

	auto feeder = parseFilmFormatMotor(format, "feeder");
	auto filmdrive = parseFilmFormatMotor(format, "filmdrive");
	auto pickup = parseFilmFormatMotor(format, "pickup");
	if (!feeder || !filmdrive || !pickup)
		return std::nullopt;

	FilmFormatConfig cfg;
	cfg.feeder = *feeder;
	cfg.filmdrive = *filmdrive;
	cfg.pickup = *pickup;
	return cfg;
}

std::optional<std::vector<std::string>>
HardwareConfig::listFilmFormatNames(const std::string &path) {
	std::ifstream in(path);
	if (!in)
		return std::nullopt;

	nlohmann::json root;
	try {
		in >> root;
	} catch (const nlohmann::json::exception &) {
		return std::nullopt;
	}
	if (!root.is_object() || !root.contains("filmFormats") ||
	    !root["filmFormats"].is_object()) {
		return std::nullopt;
	}

	std::vector<std::string> names;
	for (auto it = root["filmFormats"].begin(); it != root["filmFormats"].end(); ++it)
		names.push_back(it.key());
	return names;
}

bool HardwareConfig::saveMotorInverts(const std::string &path, bool feeder, bool filmdrive,
				       bool pickup) {
	nlohmann::ordered_json root;
	if (!readJsonObject(path, root))
		return false;
	const std::pair<const char *, bool> inverts[3] = {
		{"feeder", feeder}, {"filmdrive", filmdrive}, {"pickup", pickup}};
	for (const auto &[key, invert] : inverts) {
		if (!root.contains(key) || !root[key].is_object())
			return false;
		root[key]["invert"] = invert;
	}
	return writeJsonAtomic(path, root);
}

} // namespace hqcore
