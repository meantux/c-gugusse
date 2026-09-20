#pragma once

#include <optional>
#include <string>

struct gpiod_line_request;

namespace hqcore {

// Thin RAII wrapper over ONE requested GPIO line using the libgpiod 2.x
// API (Debian 13 "trixie" and later ship 2.x; the 1.x gpiod_chip_get_line()
// / gpiod_line_set_value() API this project originally used no longer
// exists there).
//
// The gpiochip is found by label rather than a hardcoded /dev/gpiochipN
// path, since chip numbering isn't guaranteed stable across boots. The
// label differs per Pi model: "pinctrl-rp1" on a Pi 5 (RP1 south bridge),
// "pinctrl-bcm2711" on a Pi 4 / CM4, "pinctrl-bcm2835" on older boards.
// Line offsets are the plain BCM GPIO numbers on all of them, so
// hardwarecfg.json's pin numbers work unchanged across Pi models.
class GpioLine {
public:
	enum class Bias { None, PullUp, PullDown };

	// Path (e.g. "/dev/gpiochip0") of the Pi's main GPIO header chip, or
	// std::nullopt if none of the known labels is present.
	static std::optional<std::string> findHeaderChipPath();

	static std::optional<GpioLine> requestOutput(const std::string &chipPath,
						      unsigned int offset, bool initialValue,
						      const char *consumer);
	static std::optional<GpioLine> requestInput(const std::string &chipPath,
						     unsigned int offset, Bias bias,
						     const char *consumer);

	~GpioLine();
	GpioLine(GpioLine &&other) noexcept;
	GpioLine &operator=(GpioLine &&other) noexcept;
	GpioLine(const GpioLine &) = delete;
	GpioLine &operator=(const GpioLine &) = delete;

	void set(int value);
	// 0 or 1 (an error reads as 0).
	int get() const;

private:
	GpioLine(gpiod_line_request *request, unsigned int offset);

	gpiod_line_request *request_ = nullptr;
	unsigned int offset_ = 0;
};

} // namespace hqcore
