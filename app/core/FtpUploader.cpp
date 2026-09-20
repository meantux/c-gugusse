#include "FtpUploader.h"

#include <curl/curl.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <vector>

namespace hqcore {

FtpUploader::FtpUploader(FtpConfig config, std::string watchDir, std::string projectName)
	: config_(std::move(config)), watchDir_(std::move(watchDir)),
	  projectName_(std::move(projectName)) {
	curl_global_init(CURL_GLOBAL_DEFAULT);
	thread_ = std::thread(&FtpUploader::loop, this);
}

FtpUploader::~FtpUploader() {
	running_ = false;
	if (thread_.joinable())
		thread_.join();
	curl_global_cleanup();
}

void FtpUploader::setProjectName(std::string name) {
	std::lock_guard<std::mutex> lock(projectMutex_);
	projectName_ = std::move(name);
}

std::string FtpUploader::currentProjectName() const {
	std::lock_guard<std::mutex> lock(projectMutex_);
	return projectName_;
}

std::string FtpUploader::lastStatus() const {
	std::lock_guard<std::mutex> lock(statusMutex_);
	return lastStatus_;
}

void FtpUploader::setLastStatus(std::string status) {
	std::lock_guard<std::mutex> lock(statusMutex_);
	lastStatus_ = std::move(status);
}

bool FtpUploader::uploadOne(const std::string &filePath, const std::string &fileName,
			    const std::string &projectName) {
	FILE *fp = fopen(filePath.c_str(), "rb");
	if (!fp) {
		setLastStatus("FAILED: " + fileName + " (could not open local file)");
		return false;
	}
	fseek(fp, 0, SEEK_END);
	const long fileSize = ftell(fp);
	fseek(fp, 0, SEEK_SET);

	CURL *curl = curl_easy_init();
	if (!curl) {
		fclose(fp);
		setLastStatus("FAILED: " + fileName + " (curl_easy_init failed)");
		return false;
	}

	const std::string url = config_.baseUrl() + "/" + projectName + "/" + fileName;

	curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
	curl_easy_setopt(curl, CURLOPT_USERNAME, config_.user.c_str());
	curl_easy_setopt(curl, CURLOPT_PASSWORD, config_.passwd.c_str());
	curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
	// Auto-create the <path>/<project> directory on the server if it
	// doesn't exist yet - avoids replicating
	// ../GugusseRoller's manual "list dir, create via dummy file if
	// missing" dance from transferMovieFTP.bash.
	curl_easy_setopt(curl, CURLOPT_FTP_CREATE_MISSING_DIRS,
			  static_cast<long>(CURLFTP_CREATE_DIR));
	curl_easy_setopt(curl, CURLOPT_READDATA, fp);
	curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE,
			  static_cast<curl_off_t>(fileSize));
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
	curl_easy_setopt(curl, CURLOPT_FTP_RESPONSE_TIMEOUT, 30L);

	const CURLcode res = curl_easy_perform(curl);
	curl_easy_cleanup(curl);
	fclose(fp);

	if (res == CURLE_OK) {
		setLastStatus("OK: " + fileName);
		return true;
	}
	setLastStatus("FAILED: " + fileName + " (" + curl_easy_strerror(res) + ")");
	return false;
}

void FtpUploader::loop() {
	namespace fs = std::filesystem;

	while (running_) {
		std::error_code ec;
		fs::create_directories(watchDir_, ec);

		std::vector<fs::path> files;
		for (const auto &entry : fs::directory_iterator(watchDir_, ec)) {
			if (entry.is_regular_file())
				files.push_back(entry.path());
		}
		std::sort(files.begin(), files.end());

		for (const auto &file : files) {
			if (!running_)
				break;
			const std::string project = currentProjectName();
			if (uploadOne(file.string(), file.filename().string(), project))
				std::remove(file.string().c_str());
		}

		if (files.empty())
			std::this_thread::sleep_for(std::chrono::seconds(1));
	}
}

} // namespace hqcore
