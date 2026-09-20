#include "LightController.h"

namespace hqcore {

namespace {
constexpr unsigned int kRedLine = 17;
constexpr unsigned int kGreenLine = 27;
constexpr unsigned int kBlueLine = 22;
constexpr char kConsumer[] = "gugusse-lights";
} // namespace

std::optional<LightController> LightController::open() {
	const auto chipPath = GpioLine::findHeaderChipPath();
	if (!chipPath)
		return std::nullopt;

	auto red = GpioLine::requestOutput(*chipPath, kRedLine, false, kConsumer);
	auto green = GpioLine::requestOutput(*chipPath, kGreenLine, false, kConsumer);
	auto blue = GpioLine::requestOutput(*chipPath, kBlueLine, false, kConsumer);
	if (!red || !green || !blue)
		return std::nullopt;

	return LightController(std::move(*red), std::move(*green), std::move(*blue));
}

LightController::LightController(GpioLine red, GpioLine green, GpioLine blue)
	: red_(std::move(red)), green_(std::move(green)), blue_(std::move(blue)) {}

void LightController::set(Color color) {
	int r = 0, g = 0, b = 0;
	switch (color) {
	case Color::Off:
		break;
	case Color::White:
		r = g = b = 1;
		break;
	case Color::Red:
		r = 1;
		break;
	case Color::Green:
		g = 1;
		break;
	case Color::Blue:
		b = 1;
		break;
	}
	red_.set(r);
	green_.set(g);
	blue_.set(b);
	current_ = color;
}

} // namespace hqcore
