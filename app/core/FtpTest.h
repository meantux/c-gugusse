#pragma once

#include "FtpConfig.h"

#include <string>

namespace hqcore {

// Checks that `config` really works for uploading: connects, logs in,
// creates a scratch directory under the configured base path, uploads a
// small file into it, then deletes both again - the same steps as
// ../GugusseRoller's "Test FTP settings" button. Blocking (can take up
// to the connect/response timeouts when the server is unreachable), so
// call it off the GUI thread.
//
// Returns an empty string on success, otherwise a human-readable
// description of what failed.
std::string testFtpConnection(const FtpConfig &config);

} // namespace hqcore
