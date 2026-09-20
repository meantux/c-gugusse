#pragma once

#include <optional>
#include <string>

namespace hqcore {

// FTP server settings, as read from ftp.json - same file and field names
// as ../GugusseRoller's ftp.json ("server"/"user"/"passwd"/"path"), so an
// existing installation's credentials work unmodified.
struct FtpConfig {
	std::string server;
	std::string user;
	std::string passwd;
	std::string path; // server-side base directory, e.g. "/Capture"

	// Loads from `path`. Returns std::nullopt if the file is missing,
	// malformed, or any field is absent - there's no safe made-up
	// default for FTP credentials.
	static std::optional<FtpConfig> load(const std::string &path);

	// Like load(), but for editing: never fails - a missing file/field
	// (or an unparseable file) just leaves the corresponding field empty.
	static FtpConfig loadForEditing(const std::string &path);

	// Read-modify-write of the four fields in `path`; any other keys in
	// the file are preserved. Returns false on failure (including an
	// unparseable existing file, which is left untouched).
	bool save(const std::string &path) const;

	// The server-side base directory, normalised: "/Capture" for
	// "/Capture", "Capture/" or "Capture"; "" for ""/"." (login
	// directory). Never ends in a slash.
	std::string basePath() const;

	// "ftp://<server><basePath()>", e.g. "ftp://192.168.1.5/Capture".
	// Append "/<name>" to it. (A single slash after the host means
	// "relative to the login directory" in an FTP URL - which is also
	// how FtpUploader has always addressed it.)
	std::string baseUrl() const;
};

} // namespace hqcore
