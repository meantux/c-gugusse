#include "FrameIndex.h"

#include <curl/curl.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <sstream>

namespace hqcore {

namespace fs = std::filesystem;

namespace {

std::string lowered(std::string s) {
	std::transform(s.begin(), s.end(), s.begin(),
		       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return s;
}

// The frame number in `name`, or -1 if it isn't a frame file.
long frameNumber(const std::string &path, const std::vector<std::string> &extensions) {
	const auto slash = path.find_last_of('/');
	const std::string name = slash == std::string::npos ? path : path.substr(slash + 1);

	const auto dot = name.find('.');
	if (dot == std::string::npos)
		return -1;
	const auto lastDot = name.find_last_of('.');
	const std::string ext = lowered(name.substr(lastDot + 1));
	if (std::find(extensions.begin(), extensions.end(), ext) == extensions.end())
		return -1;

	std::string stem = name.substr(0, dot);
	stem = stem.substr(0, stem.find('_'));
	if (stem.empty() || stem.size() > 9 ||
	    !std::all_of(stem.begin(), stem.end(),
			 [](unsigned char c) { return std::isdigit(c); })) {
		return -1;
	}
	return std::stol(stem);
}

std::vector<std::string> listLocal(const fs::path &dir) {
	std::vector<std::string> names;
	std::error_code ec;
	for (const auto &entry : fs::directory_iterator(dir, ec)) {
		if (entry.is_regular_file(ec))
			names.push_back(entry.path().filename().string());
	}
	return names;
}

size_t appendToString(char *data, size_t size, size_t nmemb, void *userdata) {
	static_cast<std::string *>(userdata)->append(data, size * nmemb);
	return size * nmemb;
}

} // namespace

int nextFrameIndex(const std::vector<std::string> &names,
		   const std::vector<std::string> &extensions) {
	long highest = -1;
	for (const auto &name : names)
		highest = std::max(highest, frameNumber(name, extensions));
	return static_cast<int>(highest + 1);
}

IndexLookup lookupNextIndexLocal(const std::string &basePath, const std::string &project,
				 const std::string &pendingDir,
				 const std::vector<std::string> &extensions) {
	IndexLookup result;
	std::error_code ec;
	if (!fs::is_directory(basePath, ec)) {
		result.error = basePath + " not found (drive not mounted?)";
		return result;
	}

	fs::path dir = fs::path(basePath);
	if (!project.empty())
		dir /= project;

	std::vector<std::string> names;
	result.existed = fs::is_directory(dir, ec);
	if (result.existed)
		names = listLocal(dir);
	if (!pendingDir.empty()) {
		const auto pending = listLocal(pendingDir);
		names.insert(names.end(), pending.begin(), pending.end());
	}

	result.ok = true;
	result.next = nextFrameIndex(names, extensions);
	return result;
}

IndexLookup lookupNextIndexFtp(const FtpConfig &config, const std::string &project,
			       const std::string &pendingDir,
			       const std::vector<std::string> &extensions) {
	IndexLookup result;
	if (config.server.empty()) {
		result.error = "no FTP server configured";
		return result;
	}

	curl_global_init(CURL_GLOBAL_DEFAULT);
	struct GlobalCleanup {
		~GlobalCleanup() { curl_global_cleanup(); }
	} globalCleanup;

	CURL *curl = curl_easy_init();
	if (!curl) {
		result.error = "curl_easy_init failed";
		return result;
	}

	std::string listing;
	char errorBuffer[CURL_ERROR_SIZE] = {};
	// The trailing slash makes curl treat the URL as a directory to list.
	std::string url = config.baseUrl();
	if (!project.empty())
		url += "/" + project;
	url += "/";
	curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
	curl_easy_setopt(curl, CURLOPT_USERNAME, config.user.c_str());
	curl_easy_setopt(curl, CURLOPT_PASSWORD, config.passwd.c_str());
	curl_easy_setopt(curl, CURLOPT_DIRLISTONLY, 1L); // names only (NLST)
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, appendToString);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &listing);
	// One overall limit (CURLOPT_TIMEOUT) so the countdown shown to the
	// operator is the true worst case; the others just don't cut it shorter.
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, static_cast<long>(kFtpLookupTimeoutSeconds));
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, static_cast<long>(kFtpLookupTimeoutSeconds));
	curl_easy_setopt(curl, CURLOPT_FTP_RESPONSE_TIMEOUT,
			 static_cast<long>(kFtpLookupTimeoutSeconds));
	curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errorBuffer);
	const CURLcode res = curl_easy_perform(curl);
	curl_easy_cleanup(curl);

	std::vector<std::string> names;
	if (res == CURLE_OK) {
		result.existed = true;
		std::istringstream lines(listing);
		std::string line;
		while (std::getline(lines, line)) {
			if (!line.empty() && line.back() == '\r')
				line.pop_back();
			if (!line.empty())
				names.push_back(line);
		}
	} else if (res == CURLE_REMOTE_ACCESS_DENIED || res == CURLE_REMOTE_FILE_NOT_FOUND ||
		   res == CURLE_FTP_COULDNT_RETR_FILE) {
		// Couldn't enter the directory (doesn't exist yet), or the
		// server answered an empty-directory listing with an error
		// (some servers do: "450 No files found") - either way,
		// there are no frames there.
		result.existed = false;
	} else {
		result.error = curl_easy_strerror(res);
		if (errorBuffer[0] != '\0')
			result.error += std::string(": ") + errorBuffer;
		return result;
	}

	if (!pendingDir.empty()) {
		const auto pending = listLocal(pendingDir);
		names.insert(names.end(), pending.begin(), pending.end());
	}

	result.ok = true;
	result.next = nextFrameIndex(names, extensions);
	return result;
}

} // namespace hqcore
