#pragma once

#include <algorithm>
#include <vector>

namespace hqcore {

// Recalibrates a moveTriangleUntilSensor() triangle's peak "speed" from
// one cycle to the next, aiming for the sensor to trigger right at
// targetTimeSeconds: too early (before the ramp even reaches speed2)
// means we were too fast and should slow down; too late (already
// coasting at speed2) means we were too slow and should speed up.
//
// A single cycle's timing is a noisy thing to react to outright - arm/
// hole position wobble, mechanical slop - so this averages over a small
// rolling window (see kHistoryWindow) before applying one correction,
// then starts collecting a fresh window rather than reacting again
// immediately. The correction is deliberately asymmetric: capped small
// when speeding up (overshoot risks a stall/skipped steps - a expensive
// mistake), allowed larger when slowing down (always the safe
// direction). Slowing down may go all the way to the motor's minSpeed,
// below the configured speed2 if need be (see currentSpeed2()). The very
// first cycle after construction is discarded outright rather than folded
// into the average - right after a fresh start, the arm/hole position
// isn't representative yet.
class SpeedAdapter {
public:
	SpeedAdapter(double initialSpeed, double speed2, double minSpeed, double maxSpeed,
		     double targetTimeSeconds);

	// Records one cycle's actual elapsed time (moveTriangleUntilSensor's
	// MotionOutcome::elapsedSeconds - only call this for a
	// MotionResult::Success outcome) and returns the speed to use for
	// the next cycle. Returns currentSpeed() unchanged until a full
	// window of trustworthy samples has accumulated.
	double recordCycleAndGetNextSpeed(double elapsedSeconds);

	double currentSpeed() const { return speed_; }

	// The speed2 to pair with currentSpeed() in moveTriangleUntilSensor():
	// the configured speed2, or currentSpeed() itself if recalibration
	// has pushed the speed below it (a triangle whose "peak" is under its
	// own base makes no sense, so the base follows the peak down). Purely
	// derived - never persisted, and it goes back to the configured
	// speed2 on its own if the speed later climbs back above it.
	double currentSpeed2() const { return std::min(speed2_, speed_); }

private:
	double speed_;
	double speed2_;
	double minSpeed_;
	double maxSpeed_;
	double targetTimeSeconds_;
	std::vector<double> samples_;
	bool firstCycleSeen_ = false;
};

} // namespace hqcore
