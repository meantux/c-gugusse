#include "AppPaths.h"

#include <sys/stat.h>

#include <cstdlib>
#include <filesystem>
#include <system_error>
#include <unistd.h>

namespace fs = std::filesystem;

namespace hqcore {

namespace {

// Files that live in the config directory, each seeded from defaults/<name>.
constexpr const char *kConfigFiles[] = {
    "ftp.json",
    "hardwarecfg.json",
    "preferences.json",
    "hq-camera-settings.json",
};

bool isDataDir(const fs::path &dir) {
	std::error_code ec;
	return fs::is_directory(dir / "assets", ec) && fs::is_directory(dir / "defaults", ec);
}

fs::path exeDir() {
	std::error_code ec;
	const fs::path exe = fs::read_symlink("/proc/self/exe", ec);
	return ec ? fs::path() : exe.parent_path();
}

} // namespace

std::string configDir() {
	if (const char *xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg)
		return (fs::path(xdg) / "c-gugusse").string();
	if (const char *home = std::getenv("HOME"); home && *home)
		return (fs::path(home) / ".config" / "c-gugusse").string();
	return "c-gugusse-config"; // no HOME at all: keep working, relative to cwd
}

std::string dataDir() {
	static const std::string dir = [] {
		if (const char *env = std::getenv("C_GUGUSSE_DATA_DIR"); env && *env)
			return std::string(env);
		const fs::path exe = exeDir();
		if (!exe.empty()) {
			// <repo>/app/build/c-gugusse  ->  <repo>
			const fs::path source = (exe / ".." / "..").lexically_normal();
			if (isDataDir(source))
				return source.string();
		}
#ifdef C_GUGUSSE_DATA_DIR
		return std::string(C_GUGUSSE_DATA_DIR);
#else
		return std::string(".");
#endif
	}();
	return dir;
}

std::string configFile(const std::string &name) {
	return (fs::path(configDir()) / name).string();
}

std::string assetFile(const std::string &relativePath) {
	return (fs::path(dataDir()) / "assets" / relativePath).string();
}

SeedResult seedConfigFiles() {
	SeedResult result;
	result.dir = configDir();

	std::error_code ec;
	fs::create_directories(result.dir, ec);
	if (ec) {
		result.error = "cannot create " + result.dir + ": " + ec.message();
		return result;
	}

	for (const char *name : kConfigFiles) {
		const fs::path target = fs::path(result.dir) / name;
		if (fs::exists(target, ec))
			continue;

		const fs::path source = fs::path(dataDir()) / "defaults" / name;
		// Copy to a temp name and rename so a half-written file is never
		// mistaken for a real config.
		const fs::path tmp = fs::path(result.dir) / (std::string(name) + ".tmp");
		fs::copy_file(source, tmp, fs::copy_options::overwrite_existing, ec);
		if (!ec) {
			// ftp.json holds a password: keep it private to the user.
			::chmod(tmp.c_str(), S_IRUSR | S_IWUSR);
			fs::rename(tmp, target, ec);
		}
		if (ec) {
			result.error += (result.error.empty() ? "" : "; ") +
					std::string("cannot create ") + target.string() + " from " +
					source.string() + ": " + ec.message();
		}
	}
	return result;
}

} // namespace hqcore
