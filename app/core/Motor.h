#pragma once

#include "Gpio.h"
#include "HardwareConfig.h"

#include <atomic>
#include <optional>
#include <thread>

namespace hqcore {

enum class MotorDirection { Cw, Ccw };

// Result of a sensor-triggered move (moveTriangleUntilSensor/
// advanceUntilSensor). `steps` is how many steps that specific call
// actually took (regardless of outcome) - callers use it to detect a
// "short fault": the sensor triggering suspiciously early/immediately
// across many consecutive attempts (see CaptureWindow's fault
// detection, which owns the ignoreInitial/rolling-count comparison since
// that's a film-format-specific policy, not something Motor itself
// should know about).
enum class MotionResult { Success, Aborted, LongFault };
struct MotionOutcome {
	MotionResult result = MotionResult::Success;
	long steps = 0;
	// Wall-clock time from the start of the move to the sensor
	// triggering - only meaningful for MotionResult::Success (0 for
	// Aborted/LongFault, where the sensor never genuinely triggered).
	// Feeds SpeedAdapter's targetTime recalibration for
	// moveTriangleUntilSensor().
	double elapsedSeconds = 0.0;
};

// A simple GPIO stepper motor driver for MANUAL JOG controls only:
// enable/disable, and press-and-hold CW/CCW jogging that linearly ramps
// from the configured minSpeed to maxSpeed over kRampSeconds (10s). On
// release, motors opened with decelerateOnStop=true ramp back down to
// minSpeed over kDecelSeconds (0.2s) before stopping - feeder/pickup want
// this (reel inertia/tension makes an instant stop harsh); motors opened
// with decelerateOnStop=false (filmdrive) stop immediately, since that's
// the desired feel for the sprocket-driven transport.
//
// Deliberately NOT a port of ../GugusseRoller's TrinamicSilentMotor: that
// class's move()/tick() logic targets a fixed step count expecting a
// stop-sensor pulse before a fault threshold (frame-detection for the
// film transport, with auto speed calibration) - none of that applies to
// "hold a button to jog the motor a bit", so this is a from-scratch,
// intentionally simpler implementation, not a reuse of that machinery.
//
// Wiring/limits come from HardwareConfig (hardwarecfg.json) - same file
// and field names as ../GugusseRoller, so existing wiring config works
// unmodified. Uses libgpiod 2.x through core/Gpio.h (the Pi's header
// gpiochip is found by label, so this works on Pi 4 and Pi 5 alike).
class Motor {
public:
	// startEnabled controls the very first electrical state driven onto
	// the enable pin, at the moment this process claims the line - not
	// just the initial value of isEnabled(). Motors whose real-world
	// tension/position matters (feeder/pickup holding film tension)
	// should pass true so they're never driven disabled even
	// momentarily during startup, which would release that tension.
	static std::optional<Motor> open(MotorConfig config, bool decelerateOnStop = true,
					  bool startEnabled = false);

	~Motor();
	Motor(Motor &&other) noexcept;
	Motor &operator=(Motor &&other) noexcept;
	Motor(const Motor &) = delete;
	Motor &operator=(const Motor &) = delete;

	const MotorConfig &config() const { return config_; }

	// Flips the direction-inversion flag (see MotorConfig::invert) for
	// all later setDirection()/move calls. Not thread-safe against a
	// move in progress - only call while the motor is idle.
	void setInvert(bool invert) { config_.invert = invert; }

	// pinEnable is active-low (0 = enabled), matching
	// TrinamicSilentMotor.enable()/disable() in ../GugusseRoller.
	void enable();
	void disable();
	bool isEnabled() const { return enabled_; }

	// Starts a background jog thread ramping minSpeed->maxSpeed over the
	// acceleration ramp in `direction`; call stopJog() to stop (e.g. on
	// button release). Calling startJog() while already jogging restarts
	// the ramp in the new direction. Safe to call regardless of enable
	// state - like the real hardware, step pulses while disabled simply
	// have no effect.
	void startJog(MotorDirection direction);

	// Requests a stop and blocks until the motor has actually stopped -
	// immediately if decelerateOnStop is false, otherwise after the
	// deceleration ramp completes (bounded by kDecelSeconds, so this
	// blocks the caller for at most that long).
	void stopJog();

	// True if the sensor currently reads its configured triggered state
	// (MotorConfig::stopState).
	bool sensorTriggered() const;

	// The following three are BLOCKING calls for the automated capture
	// sequence (see CaptureWindow) - unlike startJog()/stopJog(), they
	// don't spawn their own thread; call them from whatever background
	// thread is driving the sequence (two motors moving "simultaneously"
	// just means calling these from two threads and joining both).
	// `abort` is polled every step; when set, the motor stops
	// immediately (no decel, regardless of which method) and the call
	// returns false - this is the emergency-stop path.

	// Runs a symmetric triangular speed profile - speed2->speed over
	// targetTimeSeconds/2, then speed->speed2 over the remaining half -
	// in `direction`, checking the sensor every step. The instant it
	// triggers, decelerates from whatever speed it's currently at down
	// to 0 over kSensorStopDecelSeconds and returns Success. If
	// targetTimeSeconds elapses with no trigger, continues running at
	// speed2 (open-ended) until the sensor triggers or
	// faultThresholdSteps total steps have elapsed, at which point it
	// stops abruptly and returns LongFault (sensor never triggered).
	MotionOutcome moveTriangleUntilSensor(MotorDirection direction, double speed,
					       double speed2, double targetTimeSeconds,
					       long faultThresholdSteps,
					       std::atomic<bool> &abort);

	// Advances exactly `steps` steps in `direction`: first 10% ramp
	// speed2->speed, middle 80% hold at speed, last 10% ramp back down
	// to speed2. The sensor is ignored entirely. Returns false (motor
	// left stopped where it was) only if `abort` was set.
	bool advanceFixedSteps(MotorDirection direction, long steps, double speed,
				double speed2, std::atomic<bool> &abort);

	// Advances in `direction` at constant `speed2` (no ramp) until the
	// sensor triggers, then stops immediately (no decel - same instant
	// feel as filmdrive's decelerateOnStop=false). Returns LongFault if
	// faultThresholdSteps elapse with no trigger, or Aborted if `abort`
	// fires.
	MotionOutcome advanceUntilSensor(MotorDirection direction, double speed2,
					  long faultThresholdSteps, std::atomic<bool> &abort);

	// Sets the direction pin immediately. Safe to call at any time,
	// including while startContinuous()'s thread is running (a stepper
	// driver just reverses on the next pulse - there's no interlock,
	// same as the real hardware); an abrupt reversal at nonzero speed
	// may stall/rattle the motor, which for manual bench testing
	// is exactly the kind of behavior being
	// probed for, not something to guard against here.
	void setDirection(MotorDirection direction);

	// Starts a background thread for continuous, live-adjustable-speed
	// running - for manual bench testing to find real physical speed
	// limits, distinct from startJog()'s
	// fixed 10s accel-to-maxSpeed ramp and the blocking sequence
	// primitives above. The thread idles (step pin held low) at speed 0
	// until setSpeed() requests otherwise; call stopContinuous() to
	// actually stop it. Calling this while already running restarts the
	// thread idle at 0, same as startJog().
	void startContinuous(MotorDirection direction);

	// Requests a new target speed (steps/sec; 0 means idle/stopped).
	// startContinuous()'s thread ramps linearly from whatever speed
	// it's currently at to this target over kSpeedChangeSeconds, then
	// holds - regardless of how big the jump is (same fixed-duration
	// convention as kDecelSeconds elsewhere in this file, not a
	// fixed-rate ramp). Safe to call repeatedly/rapidly; each call just
	// retargets the ramp from the thread's current instantaneous speed.
	// No-op if startContinuous() hasn't been called (or after
	// stopContinuous()).
	void setSpeed(double stepsPerSecond);

	// Ramps down to 0 over kSpeedChangeSeconds (if not already there)
	// and joins the thread. Safe to call even if startContinuous() was
	// never called.
	void stopContinuous();

	// The continuous thread's actual instantaneous speed right now -
	// mid-ramp while a setSpeed() change is still catching up, equal to
	// the last requested target once it's settled. 0 if
	// startContinuous() was never called or has since stopped.
	double continuousSpeed() const { return continuousCurrentSpeed_.load(); }

private:
	explicit Motor(MotorConfig config, bool decelerateOnStop, bool startEnabled,
		       GpioLine enableLine, GpioLine directionLine, GpioLine stepLine,
		       GpioLine sensorLine);

	MotorConfig config_;
	bool decelerateOnStop_ = true;
	GpioLine enableLine_;
	GpioLine directionLine_;
	GpioLine stepLine_;
	GpioLine sensorLine_;
	bool enabled_ = false;

	std::thread jogThread_;
	std::atomic<bool> jogRunning_{false};

	std::thread continuousThread_;
	std::atomic<bool> continuousRunning_{false};
	std::atomic<double> continuousTargetSpeed_{0.0};
	// Thread's own instantaneous speed, published for stopContinuous()
	// to poll while waiting out the decel-to-0 ramp.
	std::atomic<double> continuousCurrentSpeed_{0.0};
};

} // namespace hqcore
