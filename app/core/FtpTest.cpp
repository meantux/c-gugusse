#include "FtpTest.h"

#include <curl/curl.h>

#include <chrono>
#include <cstring>

namespace hqcore {

namespace {

struct UploadSource {
	std::string data;
	size_t offset = 0;
};

size_t readFromMemory(char *buffer, size_t size, size_t nitems, void *userdata) {
	auto *source = static_cast<UploadSource *>(userdata);
	const size_t room = size * nitems;
	const size_t left = source->data.size() - source->offset;
	const size_t n = left < room ? left : room;
	std::memcpy(buffer, source->data.data() + source->offset, n);
	source->offset += n;
	return n;
}

std::string describe(CURLcode code, const char *errorBuffer) {
	std::string text = curl_easy_strerror(code);
	if (errorBuffer[0] != '\0') {
		text += ": ";
		text += errorBuffer;
	}
	return text;
}

void applyCommonOptions(CURL *curl, const FtpConfig &config, char *errorBuffer) {
	curl_easy_setopt(curl, CURLOPT_USERNAME, config.user.c_str());
	curl_easy_setopt(curl, CURLOPT_PASSWORD, config.passwd.c_str());
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
	curl_easy_setopt(curl, CURLOPT_FTP_RESPONSE_TIMEOUT, 30L);
	curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errorBuffer);
}

} // namespace

std::string testFtpConnection(const FtpConfig &config) {
	if (config.server.empty())
		return "No FTP server address configured.";

	curl_global_init(CURL_GLOBAL_DEFAULT);
	struct GlobalCleanup {
		~GlobalCleanup() { curl_global_cleanup(); }
	} globalCleanup;

	const auto stamp = std::chrono::duration_cast<std::chrono::milliseconds>(
				   std::chrono::system_clock::now().time_since_epoch())
				   .count();
	const std::string dirName = "c-gugusse_test_" + std::to_string(stamp);
	const std::string fileName = "0000.txt";
	char errorBuffer[CURL_ERROR_SIZE] = {};

	// 1. Upload a small file, creating the scratch directory on the way.
	{
		CURL *curl = curl_easy_init();
		if (!curl)
			return "curl_easy_init failed.";
		UploadSource source{dirName, 0};
		applyCommonOptions(curl, config, errorBuffer);
		const std::string url = config.baseUrl() + "/" + dirName + "/" + fileName;
		curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
		curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
		curl_easy_setopt(curl, CURLOPT_FTP_CREATE_MISSING_DIRS,
				  static_cast<long>(CURLFTP_CREATE_DIR));
		curl_easy_setopt(curl, CURLOPT_READFUNCTION, readFromMemory);
		curl_easy_setopt(curl, CURLOPT_READDATA, &source);
		curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE,
				  static_cast<curl_off_t>(source.data.size()));
		const CURLcode res = curl_easy_perform(curl);
		curl_easy_cleanup(curl);
		if (res != CURLE_OK)
			return describe(res, errorBuffer);
	}

	// 2. Delete the file and the scratch directory again. QUOTE
	// commands run in the login directory, before curl changes into the
	// URL's directory, so they need the path relative to it.
	{
		CURL *curl = curl_easy_init();
		if (!curl)
			return "Upload worked, but curl_easy_init failed for the clean-up.";
		errorBuffer[0] = '\0';
		applyCommonOptions(curl, config, errorBuffer);
		std::string relative = config.basePath();
		if (!relative.empty())
			relative.erase(0, 1); // drop the leading '/'
		if (!relative.empty())
			relative += '/';
		const std::string dirPath = relative + dirName;
		curl_slist *commands = nullptr;
		commands = curl_slist_append(commands, ("DELE " + dirPath + "/" + fileName).c_str());
		commands = curl_slist_append(commands, ("RMD " + dirPath).c_str());
		const std::string url = config.baseUrl() + "/";
		curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
		curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
		curl_easy_setopt(curl, CURLOPT_QUOTE, commands);
		const CURLcode res = curl_easy_perform(curl);
		curl_slist_free_all(commands);
		curl_easy_cleanup(curl);
		if (res != CURLE_OK) {
			return "Upload worked, but removing the test directory \"" + dirName +
			       "\" failed: " + describe(res, errorBuffer);
		}
	}

	return {};
}

} // namespace hqcore
