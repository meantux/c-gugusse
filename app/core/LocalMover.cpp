#include "LocalMover.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <vector>

namespace hqcore {

namespace fs = std::filesystem;

LocalMover::LocalMover(std::string basePath, std::string watchDir, std::string projectName)
	: basePath_(std::move(basePath)), watchDir_(std::move(watchDir)),
	  projectName_(std::move(projectName)) {
	thread_ = std::thread(&LocalMover::loop, this);
}

LocalMover::~LocalMover() {
	running_ = false;
	if (thread_.joinable())
		thread_.join();
}

void LocalMover::setProjectName(std::string name) {
	std::lock_guard<std::mutex> lock(projectMutex_);
	projectName_ = std::move(name);
}

std::string LocalMover::currentProjectName() const {
	std::lock_guard<std::mutex> lock(projectMutex_);
	return projectName_;
}

std::string LocalMover::lastStatus() const {
	std::lock_guard<std::mutex> lock(statusMutex_);
	return lastStatus_;
}

void LocalMover::setLastStatus(std::string status) {
	std::lock_guard<std::mutex> lock(statusMutex_);
	lastStatus_ = std::move(status);
}

bool LocalMover::moveOne(const std::string &filePath, const std::string &fileName,
			 const std::string &projectName) {
	std::error_code ec;
	if (!fs::is_directory(basePath_, ec)) {
		setLastStatus("FAILED: " + fileName + " (" + basePath_ +
			      " not found - drive not mounted?)");
		return false;
	}

	fs::path destDir = fs::path(basePath_);
	if (!projectName.empty())
		destDir /= projectName;
	fs::create_directory(destDir, ec); // no-op (and no error) if it already exists
	if (ec) {
		setLastStatus("FAILED: " + fileName + " (cannot create " + destDir.string() +
			      ": " + ec.message() + ")");
		return false;
	}

	const fs::path dest = destDir / fileName;

	// Same filesystem: an atomic rename. (Not the usual case - /dev/shm is
	// a tmpfs and the destination is normally a different disk - but free
	// to try.)
	fs::rename(filePath, dest, ec);
	if (!ec) {
		setLastStatus("OK: " + fileName);
		return true;
	}

	// Different filesystem: copy under a temporary name, then rename into
	// place, so a partially-copied file never carries the real name (an
	// interrupted copy - unplugged drive, power loss - can't be mistaken
	// for a finished frame), and only then delete the source.
	const fs::path partial = destDir / (fileName + ".part");
	fs::copy_file(filePath, partial, fs::copy_options::overwrite_existing, ec);
	if (!ec)
		fs::rename(partial, dest, ec);
	if (ec) {
		std::error_code ignored;
		fs::remove(partial, ignored);
		setLastStatus("FAILED: " + fileName + " (" + ec.message() + ")");
		return false;
	}
	fs::remove(filePath, ec); // if this fails the file is just moved again next pass
	setLastStatus("OK: " + fileName);
	return true;
}

void LocalMover::loop() {
	while (running_) {
		std::error_code ec;
		fs::create_directories(watchDir_, ec);

		std::vector<fs::path> files;
		for (const auto &entry : fs::directory_iterator(watchDir_, ec)) {
			if (entry.is_regular_file())
				files.push_back(entry.path());
		}
		std::sort(files.begin(), files.end());

		bool movedAny = false;
		for (const auto &file : files) {
			if (!running_)
				break;
			if (moveOne(file.string(), file.filename().string(), currentProjectName()))
				movedAny = true;
			else
				break; // same cause will hit every file - wait before retrying
		}

		// Idle, or the last pass failed: don't spin.
		if (!movedAny) {
			for (int i = 0; i < 10 && running_; ++i)
				std::this_thread::sleep_for(std::chrono::milliseconds(100));
		}
	}
}

} // namespace hqcore
