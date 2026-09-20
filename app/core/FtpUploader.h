#pragma once

#include "FtpConfig.h"

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

namespace hqcore {

// Watches a local directory (default /dev/shm/complete) for files and
// uploads each one via FTP (libcurl) into <ftp path>/<project name>/ on
// the configured server, deleting the local file once the upload
// succeeds - matching ../GugusseRoller's sendWhileRunning.bash
// (`ncftpput ... && rm`). A failed upload is left in place and retried
// on the next pass. Files are uploaded in filename-sorted order (frame
// filenames are zero-padded, so this is also numeric order).
//
// The project subdirectory name can change while running (the operator
// may edit it mid-session) - setProjectName() is thread-safe and takes
// effect on the next file picked up, not one already in flight.
//
// Runs its own background thread from construction until destruction;
// there is no separate start()/stop().
class FtpUploader {
public:
	// `projectName` is the initial value for setProjectName(), in place
	// before the thread starts so files already waiting in the watch
	// directory at startup aren't sent to the wrong place.
	explicit FtpUploader(FtpConfig config, std::string watchDir = "/dev/shm/complete",
			     std::string projectName = {});
	~FtpUploader();
	FtpUploader(const FtpUploader &) = delete;
	FtpUploader &operator=(const FtpUploader &) = delete;

	void setProjectName(std::string name);

	// Human-readable outcome of the most recent upload attempt (for a
	// status display); empty if none has happened yet.
	std::string lastStatus() const;

private:
	void loop();
	bool uploadOne(const std::string &filePath, const std::string &fileName,
		       const std::string &projectName);
	std::string currentProjectName() const;
	void setLastStatus(std::string status);

	FtpConfig config_;
	std::string watchDir_;

	mutable std::mutex projectMutex_;
	std::string projectName_;

	mutable std::mutex statusMutex_;
	std::string lastStatus_;

	std::atomic<bool> running_{true};
	std::thread thread_;
};

} // namespace hqcore
