#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

namespace hqcore {

// The "local copy" alternative to FtpUploader: watches a local directory
// (default /dev/shm/complete) and moves each file into
// <base path>/<project name>/ - typically a directory on a USB drive -
// matching ../GugusseRoller's LocalThread. The source file is only removed
// once it is fully in place at the destination; a failed move (drive
// full, unplugged, ...) leaves it where it is and is retried on the next
// pass. Files are moved in filename-sorted order.
//
// The base path must already exist (it is never created - creating it
// would silently write to the system disk if the drive simply isn't
// mounted); the <project name> subdirectory is created as needed.
//
// The project name can change while running - setProjectName() is
// thread-safe and takes effect on the next file picked up. Runs its own
// background thread from construction until destruction.
class LocalMover {
public:
	// `projectName` is the initial value for setProjectName(), in place
	// before the thread starts so files already waiting in the watch
	// directory at startup aren't moved to the wrong place.
	explicit LocalMover(std::string basePath, std::string watchDir = "/dev/shm/complete",
			    std::string projectName = {});
	~LocalMover();
	LocalMover(const LocalMover &) = delete;
	LocalMover &operator=(const LocalMover &) = delete;

	void setProjectName(std::string name);

	// Human-readable outcome of the most recent move attempt (for a
	// status display); empty if none has happened yet.
	std::string lastStatus() const;

private:
	void loop();
	bool moveOne(const std::string &filePath, const std::string &fileName,
		     const std::string &projectName);
	std::string currentProjectName() const;
	void setLastStatus(std::string status);

	std::string basePath_;
	std::string watchDir_;

	mutable std::mutex projectMutex_;
	std::string projectName_;

	mutable std::mutex statusMutex_;
	std::string lastStatus_;

	std::atomic<bool> running_{true};
	std::thread thread_;
};

} // namespace hqcore
