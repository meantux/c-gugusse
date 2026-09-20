#pragma once

#include <string>

namespace hqcore {

// How finished frames leave /dev/shm/complete: uploaded over FTP
// (FtpUploader, credentials in ftp.json) or moved to a directory on the
// machine itself (LocalMover) - typically a USB drive.
//
// Stored in hardwarecfg.json under "saveMode" ("ftp"/"local") and
// "localFilePath", exactly like ../GugusseRoller, so both apps agree.
struct ExportSettings {
	enum class Mode { Ftp, Local };

	Mode mode = Mode::Ftp;
	std::string localPath = "/media";

	// Missing file/keys, or an unknown saveMode value, give the defaults
	// above (FTP - what the app did before there was a choice). Never
	// throws.
	static ExportSettings load(const std::string &path);

	// Read-modify-write of only the two keys in `path`; everything else
	// in the file is preserved. Returns false on failure (including an
	// unparseable existing file, which is left untouched).
	bool save(const std::string &path) const;
};

} // namespace hqcore
