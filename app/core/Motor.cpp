#include "Motor.h"

#include <algorithm>
#include <chrono>
#include <utility>

namespace hqcore {

namespace {
using Clock = std::chrono::steady_clock;

constexpr double kRampSeconds = 10.0;
constexpr double kDecelSeconds = 0.2;
constexpr double kSensorStopDecelSeconds = 0.1;
constexpr double kSpeedChangeSeconds = 0.2;
constexpr double kMinPacedSpeed = 50.0;
constexpr char kConsumer[] = "gugusse-motor";

// Sleeping for the full requested period is unreliable on this non-RT
// kernel - measured empirically (see this session's scratchpad
// gpio_throughput.cpp): a plain sleep_for() loop targeting 40kHz only
// achieved ~31% of the requested rate (a 20000 steps/sec motor would
// actually top out around 6000). Sleeping for most of the period and
// busy-spinning the last kSpinMargin closes that gap to ~100% accuracy,
// at the cost of pinning one CPU core while a motor is moving -
// acceptable since these are brief, foreground operations.
constexpr auto kSpinMargin = std::chrono::microseconds(50);

// Toggles the step pin once at `speed` steps/sec, pacing against
// `nextTick` (advanced by one period each call) - shared by every motion
// primitive below. Two toggles per step: most step/dir drivers (this
// project's included) latch a step on the STEP pin's rising edge only,
// so a full low-high-low cycle is one step.
void stepPinAt(GpioLine &stepLine, double speed, Clock::time_point &nextTick,
	       bool &pinState) {
	const auto period = std::chrono::duration_cast<Clock::duration>(
		std::chrono::duration<double>(1.0 / (speed * 2.0)));
	const auto remaining = nextTick - Clock::now();
	if (remaining > kSpinMargin)
		std::this_thread::sleep_for(remaining - kSpinMargin);
	while (Clock::now() < nextTick) {
		// spin for the final stretch - see kSpinMargin comment above
	}
	pinState = !pinState;
	stepLine.set(pinState ? 1 : 0);
	nextTick += period;
}
} // namespace

std::optional<Motor> Motor::open(MotorConfig config, bool decelerateOnStop,
				  bool startEnabled) {
	const auto chipPath = GpioLine::findHeaderChipPath();
	if (!chipPath)
		return std::nullopt;

	// Step pin low; enable pin driven straight to its startEnabled state
	// (0=enabled, active-low) the instant this process claims the line -
	// for motors holding real film tension (feeder/pickup), passing
	// startEnabled=true means there is no disabled moment at all during
	// startup, not even a brief one, unlike always starting disabled
	// (which matched TrinamicSilentMotor's own initial GPIO setup but
	// drops tension on every app launch).
	auto enableLine = GpioLine::requestOutput(
		*chipPath, static_cast<unsigned int>(config.pinEnable), !startEnabled, kConsumer);
	auto directionLine = GpioLine::requestOutput(
		*chipPath, static_cast<unsigned int>(config.pinDirection), false, kConsumer);
	auto stepLine = GpioLine::requestOutput(
		*chipPath, static_cast<unsigned int>(config.pinStep), false, kConsumer);
	if (!enableLine || !directionLine || !stepLine)
		return std::nullopt;

	GpioLine::Bias bias = GpioLine::Bias::None;
	if (config.pullUp)
		bias = GpioLine::Bias::PullUp;
	else if (config.pullDown)
		bias = GpioLine::Bias::PullDown;
	auto sensorLine = GpioLine::requestInput(
		*chipPath, static_cast<unsigned int>(config.stopPin), bias, kConsumer);
	if (!sensorLine)
		return std::nullopt;

	return Motor(std::move(config), decelerateOnStop, startEnabled, std::move(*enableLine),
		     std::move(*directionLine), std::move(*stepLine), std::move(*sensorLine));
}

Motor::Motor(MotorConfig config, bool decelerateOnStop, bool startEnabled, GpioLine enableLine,
	     GpioLine directionLine, GpioLine stepLine, GpioLine sensorLine)
	: config_(std::move(config)), decelerateOnStop_(decelerateOnStop),
	  enableLine_(std::move(enableLine)), directionLine_(std::move(directionLine)),
	  stepLine_(std::move(stepLine)), sensorLine_(std::move(sensorLine)),
	  enabled_(startEnabled) {}

Motor::Motor(Motor &&other) noexcept
	: config_(std::move(other.config_)), decelerateOnStop_(other.decelerateOnStop_),
	  enableLine_(std::move(other.enableLine_)),
	  directionLine_(std::move(other.directionLine_)), stepLine_(std::move(other.stepLine_)),
	  sensorLine_(std::move(other.sensorLine_)), enabled_(other.enabled_),
	  jogThread_(std::move(other.jogThread_)), jogRunning_(other.jogRunning_.load()),
	  continuousThread_(std::move(other.continuousThread_)),
	  continuousRunning_(other.continuousRunning_.load()),
	  continuousTargetSpeed_(other.continuousTargetSpeed_.load()),
	  continuousCurrentSpeed_(other.continuousCurrentSpeed_.load()) {}

Motor &Motor::operator=(Motor &&other) noexcept {
	if (this != &other) {
		stopJog();
		stopContinuous();
		config_ = std::move(other.config_);
		decelerateOnStop_ = other.decelerateOnStop_;
		enableLine_ = std::move(other.enableLine_);
		directionLine_ = std::move(other.directionLine_);
		stepLine_ = std::move(other.stepLine_);
		sensorLine_ = std::move(other.sensorLine_);
		enabled_ = other.enabled_;
		jogThread_ = std::move(other.jogThread_);
		jogRunning_ = other.jogRunning_.load();
		continuousThread_ = std::move(other.continuousThread_);
		continuousRunning_ = other.continuousRunning_.load();
		continuousTargetSpeed_ = other.continuousTargetSpeed_.load();
		continuousCurrentSpeed_ = other.continuousCurrentSpeed_.load();
	}
	return *this;
}

Motor::~Motor() {
	stopJog();
	stopContinuous();
}

void Motor::enable() {
	enableLine_.set(0);
	enabled_ = true;
}

void Motor::disable() {
	enableLine_.set(1);
	enabled_ = false;
}

bool Motor::sensorTriggered() const {
	return sensorLine_.get() == config_.stopState;
}

void Motor::setDirection(MotorDirection direction) {
	// rev XOR invert -> pin low; matches TrinamicSilentMotor.setDirection()
	// in ../GugusseRoller exactly (ccw=rev=true there).
	const bool rev = (direction == MotorDirection::Ccw);
	const int pinValue = (config_.invert != rev) ? 0 : 1;
	directionLine_.set(pinValue);
}

void Motor::startJog(MotorDirection direction) {
	stopJog();
	setDirection(direction);

	jogRunning_ = true;
	jogThread_ = std::thread([this] {
		bool pinState = false;
		auto nextTick = Clock::now();

		// Acceleration phase: minSpeed -> maxSpeed over kRampSeconds.
		// jogRunning_ is only checked once per step (not inside the
		// wait itself), so a stop request is noticed within at most
		// one step period - negligible (well under minSpeed's ~25ms
		// worst case, far less once ramped up).
		const auto accelStart = Clock::now();
		double lastSpeed = config_.minSpeed;
		while (jogRunning_) {
			const double elapsed =
				std::chrono::duration<double>(Clock::now() - accelStart).count();
			const double t = std::min(elapsed / kRampSeconds, 1.0);
			lastSpeed = config_.minSpeed + t * (config_.maxSpeed - config_.minSpeed);
			stepPinAt(stepLine_, lastSpeed, nextTick, pinState);
		}

		// Deceleration phase (feeder/pickup): ramp back down to
		// minSpeed over kDecelSeconds before actually stopping, so
		// releasing the button doesn't slam the reel to a dead stop.
		// filmdrive (decelerateOnStop=false) skips this - an instant
		// stop is the desired feel for the sprocket-driven transport.
		if (decelerateOnStop_) {
			const auto decelStart = Clock::now();
			const double startSpeed = lastSpeed;
			double elapsed = 0.0;
			while ((elapsed = std::chrono::duration<double>(
					Clock::now() - decelStart)
						   .count()) < kDecelSeconds) {
				const double t = elapsed / kDecelSeconds;
				const double speed =
					startSpeed - t * (startSpeed - config_.minSpeed);
				stepPinAt(stepLine_, speed, nextTick, pinState);
			}
		}

		stepLine_.set(0);
	});
}

void Motor::stopJog() {
	jogRunning_ = false;
	if (jogThread_.joinable())
		jogThread_.join();
}

MotionOutcome Motor::moveTriangleUntilSensor(MotorDirection direction, double speed,
					      double speed2, double targetTimeSeconds,
					      long faultThresholdSteps,
					      std::atomic<bool> &abort) {
	setDirection(direction);
	bool pinState = false;
	auto nextTick = Clock::now();
	const auto start = Clock::now();
	const double halfTime = targetTimeSeconds / 2.0;
	long steps = 0;
	double lastSpeed = speed2;

	while (!sensorTriggered()) {
		if (abort) {
			stepLine_.set(0);
			return {MotionResult::Aborted, steps};
		}
		if (++steps > faultThresholdSteps) {
			stepLine_.set(0);
			return {MotionResult::LongFault, steps}; // sensor never triggered
		}

		const double elapsed = std::chrono::duration<double>(Clock::now() - start).count();
		if (elapsed < halfTime)
			lastSpeed = speed2 + (speed - speed2) * (elapsed / halfTime);
		else if (elapsed < targetTimeSeconds)
			lastSpeed = speed - (speed - speed2) * ((elapsed - halfTime) / halfTime);
		else
			lastSpeed = speed2; // ramp cycle done, hold at speed2 until triggered

		stepPinAt(stepLine_, lastSpeed, nextTick, pinState);
	}

	// Measured the instant the sensor triggered, before the deceleration
	// below - that's the real "time to reach the arm", what
	// SpeedAdapter's recalibration is comparing against targetTime.
	const double elapsedSeconds = std::chrono::duration<double>(Clock::now() - start).count();

	// Sensor triggered - decelerate from whatever speed we were at down
	// to 0 over kSensorStopDecelSeconds, then stop.
	const auto decelStart = Clock::now();
	double decelElapsed = 0.0;
	while ((decelElapsed = std::chrono::duration<double>(Clock::now() - decelStart).count()) <
	       kSensorStopDecelSeconds) {
		if (abort)
			break;
		const double t = decelElapsed / kSensorStopDecelSeconds;
		const double decelSpeed = std::max(lastSpeed * (1.0 - t), 1.0);
		stepPinAt(stepLine_, decelSpeed, nextTick, pinState);
	}
	stepLine_.set(0);
	return {MotionResult::Success, steps, elapsedSeconds};
}

bool Motor::advanceFixedSteps(MotorDirection direction, long steps, double speed,
			       double speed2, std::atomic<bool> &abort) {
	setDirection(direction);
	bool pinState = false;
	auto nextTick = Clock::now();

	const long rampSteps = std::max<long>(1, steps / 10);
	for (long i = 0; i < steps; ++i) {
		if (abort) {
			stepLine_.set(0);
			return false;
		}
		double stepSpeed;
		if (i < rampSteps)
			stepSpeed = speed2 + (speed - speed2) * (static_cast<double>(i) / rampSteps);
		else if (i >= steps - rampSteps)
			stepSpeed = speed - (speed - speed2) *
					     (static_cast<double>(i - (steps - rampSteps)) / rampSteps);
		else
			stepSpeed = speed;

		stepPinAt(stepLine_, stepSpeed, nextTick, pinState);
	}
	stepLine_.set(0);
	return true;
}

MotionOutcome Motor::advanceUntilSensor(MotorDirection direction, double speed2,
					 long faultThresholdSteps, std::atomic<bool> &abort) {
	setDirection(direction);
	bool pinState = false;
	auto nextTick = Clock::now();
	long steps = 0;

	while (!sensorTriggered()) {
		if (abort) {
			stepLine_.set(0);
			return {MotionResult::Aborted, steps};
		}
		if (++steps > faultThresholdSteps) {
			stepLine_.set(0);
			return {MotionResult::LongFault, steps}; // sensor never triggered
		}
		stepPinAt(stepLine_, speed2, nextTick, pinState);
	}
	// Stop immediately - no decel, matching filmdrive's instant-stop feel.
	stepLine_.set(0);
	return {MotionResult::Success, steps};
}

void Motor::startContinuous(MotorDirection direction) {
	stopContinuous();
	setDirection(direction);

	continuousTargetSpeed_ = 0.0;
	continuousCurrentSpeed_ = 0.0;
	continuousRunning_ = true;
	continuousThread_ = std::thread([this] {
		bool pinState = false;
		auto nextTick = Clock::now();
		double currentSpeed = 0.0;
		double rampFrom = 0.0;
		double rampTo = 0.0;
		double appliedTarget = 0.0;
		auto rampStart = Clock::now();

		while (continuousRunning_) {
			const double target = continuousTargetSpeed_.load();
			if (target != appliedTarget) {
				rampFrom = currentSpeed;
				rampTo = target;
				rampStart = Clock::now();
				appliedTarget = target;
			}

			const double elapsed =
				std::chrono::duration<double>(Clock::now() - rampStart).count();
			const double t = std::min(elapsed / kSpeedChangeSeconds, 1.0);
			currentSpeed = rampFrom + t * (rampTo - rampFrom);
			continuousCurrentSpeed_ = currentSpeed;

			const bool idle = std::max(rampFrom, rampTo) <= 0.0 ||
					  (t >= 1.0 && rampTo <= 0.0);
			if (idle) {
				stepLine_.set(0);
				pinState = false;
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
				// Reset the pacing clock so resuming after an idle
				// stretch doesn't see a huge backlog and burst steps
				// to "catch up".
				nextTick = Clock::now();
				continue;
			}

			// Ramping to/from 0 passes through arbitrarily tiny speeds,
			// whose step period is seconds (or minutes) long - and
			// stepPinAt() sleeps the whole period, so the motor would
			// sit dead and the thread couldn't react to a new target or
			// a stop. Never pace slower than kMinPacedSpeed (or the
			// ramp's own larger endpoint, if that is smaller).
			const double stepSpeed = std::max(
				currentSpeed, std::min(kMinPacedSpeed, std::max(rampFrom, rampTo)));
			stepPinAt(stepLine_, stepSpeed, nextTick, pinState);
		}
		stepLine_.set(0);
	});
}

void Motor::setSpeed(double stepsPerSecond) {
	if (!continuousRunning_)
		return;
	continuousTargetSpeed_ = std::max(0.0, stepsPerSecond);
}

void Motor::stopContinuous() {
	if (!continuousRunning_) {
		if (continuousThread_.joinable())
			continuousThread_.join();
		return;
	}

	continuousTargetSpeed_ = 0.0;
	// Wait out the ramp-to-0 before actually tearing down the thread,
	// same "block for at most the decel time" contract as stopJog().
	const auto waitStart = Clock::now();
	while (continuousCurrentSpeed_.load() > 0.0 &&
	       std::chrono::duration<double>(Clock::now() - waitStart).count() <
		       kSpeedChangeSeconds * 2) {
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	continuousRunning_ = false;
	if (continuousThread_.joinable())
		continuousThread_.join();
}

} // namespace hqcore
