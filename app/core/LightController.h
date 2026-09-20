#pragma once

#include "Gpio.h"

#include <optional>

namespace hqcore {

// Drives the three film-lighting LEDs via GPIO, one output line per
// color, matching ../GugusseRoller's Lights.py wiring: BCM GPIO 17 (red),
// 27 (green), 22 (blue) on the 40-pin header, active high (see
// hardwarecfg.json's "lights" section on the imx_183 branch). "White"
// drives all three simultaneously; "Off" drives none.
//
// Uses libgpiod 2.x via core/Gpio.h, which finds the Pi's header gpiochip
// by label (works on both Pi 4 and Pi 5).
//
// The application only ever uses Off/White (the HQ camera is a colour
// sensor, so no per-colour lighting); the individual colour outputs are
// still available for other uses.
class LightController {
public:
	enum class Color { Off, White, Red, Green, Blue };

	static std::optional<LightController> open();

	LightController(LightController &&) noexcept = default;
	LightController &operator=(LightController &&) noexcept = default;

	void set(Color color);
	Color current() const { return current_; }

private:
	LightController(GpioLine red, GpioLine green, GpioLine blue);

	GpioLine red_;
	GpioLine green_;
	GpioLine blue_;
	Color current_ = Color::Off;
};

} // namespace hqcore
