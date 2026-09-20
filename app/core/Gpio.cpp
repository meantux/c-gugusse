#include "Gpio.h"

#include <gpiod.h>

#include <cstring>
#include <filesystem>
#include <utility>

namespace hqcore {

namespace {
constexpr const char *kHeaderChipLabels[] = {"pinctrl-rp1", "pinctrl-bcm2711",
					      "pinctrl-bcm2835"};
} // namespace

std::optional<std::string> GpioLine::findHeaderChipPath() {
	namespace fs = std::filesystem;
	std::error_code ec;
	for (const auto &entry : fs::directory_iterator("/dev", ec)) {
		const std::string name = entry.path().filename().string();
		if (name.rfind("gpiochip", 0) != 0)
			continue;
		gpiod_chip *chip = gpiod_chip_open(entry.path().c_str());
		if (!chip)
			continue;
		std::optional<std::string> found;
		if (gpiod_chip_info *info = gpiod_chip_get_info(chip)) {
			const char *label = gpiod_chip_info_get_label(info);
			for (const char *wanted : kHeaderChipLabels) {
				if (label && std::strcmp(label, wanted) == 0)
					found = entry.path().string();
			}
			gpiod_chip_info_free(info);
		}
		gpiod_chip_close(chip);
		if (found)
			return found;
	}
	return std::nullopt;
}

namespace {
// Shared by requestOutput/requestInput - builds the request config around
// an already-populated `settings` for a single line.
gpiod_line_request *doRequest(const std::string &chipPath, unsigned int offset,
			      gpiod_line_settings *settings, const char *consumer) {
	gpiod_chip *chip = gpiod_chip_open(chipPath.c_str());
	if (!chip)
		return nullptr;

	gpiod_line_config *lineCfg = gpiod_line_config_new();
	gpiod_request_config *reqCfg = gpiod_request_config_new();
	gpiod_line_request *request = nullptr;
	if (lineCfg && reqCfg &&
	    gpiod_line_config_add_line_settings(lineCfg, &offset, 1, settings) == 0) {
		gpiod_request_config_set_consumer(reqCfg, consumer);
		request = gpiod_chip_request_lines(chip, reqCfg, lineCfg);
	}
	if (reqCfg)
		gpiod_request_config_free(reqCfg);
	if (lineCfg)
		gpiod_line_config_free(lineCfg);
	// A line request stays valid after its chip handle is closed.
	gpiod_chip_close(chip);
	return request;
}
} // namespace

std::optional<GpioLine> GpioLine::requestOutput(const std::string &chipPath,
						 unsigned int offset, bool initialValue,
						 const char *consumer) {
	gpiod_line_settings *settings = gpiod_line_settings_new();
	if (!settings)
		return std::nullopt;
	gpiod_line_settings_set_direction(settings, GPIOD_LINE_DIRECTION_OUTPUT);
	gpiod_line_settings_set_output_value(
		settings, initialValue ? GPIOD_LINE_VALUE_ACTIVE : GPIOD_LINE_VALUE_INACTIVE);
	gpiod_line_request *request = doRequest(chipPath, offset, settings, consumer);
	gpiod_line_settings_free(settings);
	if (!request)
		return std::nullopt;
	return GpioLine(request, offset);
}

std::optional<GpioLine> GpioLine::requestInput(const std::string &chipPath, unsigned int offset,
						Bias bias, const char *consumer) {
	gpiod_line_settings *settings = gpiod_line_settings_new();
	if (!settings)
		return std::nullopt;
	gpiod_line_settings_set_direction(settings, GPIOD_LINE_DIRECTION_INPUT);
	switch (bias) {
	case Bias::PullUp:
		gpiod_line_settings_set_bias(settings, GPIOD_LINE_BIAS_PULL_UP);
		break;
	case Bias::PullDown:
		gpiod_line_settings_set_bias(settings, GPIOD_LINE_BIAS_PULL_DOWN);
		break;
	case Bias::None:
		break;
	}
	gpiod_line_request *request = doRequest(chipPath, offset, settings, consumer);
	gpiod_line_settings_free(settings);
	if (!request)
		return std::nullopt;
	return GpioLine(request, offset);
}

GpioLine::GpioLine(gpiod_line_request *request, unsigned int offset)
	: request_(request), offset_(offset) {}

GpioLine::GpioLine(GpioLine &&other) noexcept
	: request_(std::exchange(other.request_, nullptr)), offset_(other.offset_) {}

GpioLine &GpioLine::operator=(GpioLine &&other) noexcept {
	if (this != &other) {
		if (request_)
			gpiod_line_request_release(request_);
		request_ = std::exchange(other.request_, nullptr);
		offset_ = other.offset_;
	}
	return *this;
}

GpioLine::~GpioLine() {
	if (request_)
		gpiod_line_request_release(request_);
}

void GpioLine::set(int value) {
	gpiod_line_request_set_value(request_, offset_,
				     value ? GPIOD_LINE_VALUE_ACTIVE : GPIOD_LINE_VALUE_INACTIVE);
}

int GpioLine::get() const {
	return gpiod_line_request_get_value(request_, offset_) == GPIOD_LINE_VALUE_ACTIVE ? 1 : 0;
}

} // namespace hqcore
