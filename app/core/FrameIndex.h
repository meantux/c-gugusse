#pragma once

#include "FtpConfig.h"

#include <string>
#include <vector>

namespace hqcore {

// Finding where frame numbering should resume when a project directory
// already holds frames (a session continued after a restart, or a project
// name reused): the next index is one past the highest frame number found
// in the destination directory - or 0 if the directory doesn't exist yet
// or holds no frames. Same idea as ../GugusseRoller's getStartPoint()
// (which takes the last file sorted by name), but takes the numeric
// maximum, so it doesn't depend on the server's listing order.
//
// Frame files are named "<digits>[_<anything>].<ext>" (e.g. "00042.dng").
// Only names with one of the given extensions count, so stray files
// (".part" leftovers, "0000.txt" test files, ...) never move the index.

struct IndexLookup {
	bool ok = false;      // false: the directory couldn't be checked (see `error`)
	bool existed = false; // the project directory was already there
	int next = 0;         // the index to start at (meaningful only if ok)
	std::string error;    // human-readable reason when !ok
};

// One past the highest frame number among `names` (bare names or paths -
// only the part after the last '/' is looked at); 0 if none match.
// `extensions` are lower-case, without the dot, e.g. {"dng", "jpg"}.
int nextFrameIndex(const std::vector<std::string> &names,
		   const std::vector<std::string> &extensions);

// Checks <basePath>/<project>/ (just <basePath>/ for an empty project).
// `pendingDir` (may be empty) is also scanned: frames still waiting there
// to be moved/uploaded into the same project count as existing, or a new
// capture could overwrite one. Fails if `basePath` itself is missing
// (drive not mounted).
IndexLookup lookupNextIndexLocal(const std::string &basePath, const std::string &project,
				 const std::string &pendingDir,
				 const std::vector<std::string> &extensions);

// How long lookupNextIndexFtp() may take in total (connect, login, listing)
// before giving up. Generous on purpose: a file server that is also the FTP
// server may need most of a minute to wake up from sleep. The GUI shows a
// countdown from this value.
constexpr int kFtpLookupTimeoutSeconds = 90;

// Same for <ftp base>/<project>/ on the FTP server, listed over a
// separate connection. Blocking (up to kFtpLookupTimeoutSeconds if the
// server is unreachable or asleep) - call off the GUI thread. A directory the
// server refuses to enter counts as not existing yet (the upload would
// fail the same way, so nothing can be overwritten); a connection or
// login failure is an error.
IndexLookup lookupNextIndexFtp(const FtpConfig &config, const std::string &project,
			       const std::string &pendingDir,
			       const std::vector<std::string> &extensions);

} // namespace hqcore
