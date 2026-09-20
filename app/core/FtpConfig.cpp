#include "FtpConfig.h"

#include "JsonFile.h"

#include <nlohmann/json.hpp>

#include <fstream>

namespace hqcore {

std::optional<FtpConfig> FtpConfig::load(const std::string &path) {
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

	if (!root.contains("server") || !root.contains("user") ||
	    !root.contains("passwd") || !root.contains("path")) {
		return std::nullopt;
	}

	FtpConfig cfg;
	try {
		cfg.server = root.at("server").get<std::string>();
		cfg.user = root.at("user").get<std::string>();
		cfg.passwd = root.at("passwd").get<std::string>();
		cfg.path = root.at("path").get<std::string>();
	} catch (const nlohmann::json::exception &) {
		return std::nullopt;
	}

	if (cfg.server.empty())
		return std::nullopt;

	return cfg;
}

FtpConfig FtpConfig::loadForEditing(const std::string &path) {
	FtpConfig cfg;
	nlohmann::ordered_json root;
	if (!readJsonObject(path, root))
		return cfg;
	auto field = [&](const char *key) {
		const auto it = root.find(key);
		return (it != root.end() && it->is_string()) ? it->get<std::string>()
							     : std::string();
	};
	cfg.server = field("server");
	cfg.user = field("user");
	cfg.passwd = field("passwd");
	cfg.path = field("path");
	return cfg;
}

bool FtpConfig::save(const std::string &path) const {
	nlohmann::ordered_json root;
	if (!readJsonObject(path, root))
		return false;
	root["server"] = server;
	root["user"] = user;
	root["passwd"] = passwd;
	root["path"] = this->path;
	return writeJsonAtomic(path, root);
}

std::string FtpConfig::basePath() const {
	std::string base = path;
	while (!base.empty() && base.back() == '/')
		base.pop_back();
	if (base == ".")
		base.clear();
	if (!base.empty() && base.front() != '/')
		base.insert(base.begin(), '/');
	return base;
}

std::string FtpConfig::baseUrl() const {
	return "ftp://" + server + basePath();
}

} // namespace hqcore
