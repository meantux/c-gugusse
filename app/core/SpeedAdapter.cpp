#include "SpeedAdapter.h"

#include <algorithm>
#include <numeric>

namespace hqcore {

namespace {
// How many cycles to average before applying one correction. Small
// enough that a run starts converging quickly (each cycle here is a full
// capture cycle - several seconds - so waiting many cycles to react
// is a real cost), large enough to smooth over single-cycle jitter.
constexpr size_t kHistoryWindow = 3;

// Correcting up (we were too slow) is capped small: overshooting into
// too-fast risks a stall/skipped steps. Correcting down (we were too
// fast) is always safe, so it's allowed a bigger step.
constexpr double kMaxSpeedUpFraction = 0.05;
constexpr double kMaxSlowDownFraction = 0.20;
} // namespace

SpeedAdapter::SpeedAdapter(double initialSpeed, double speed2, double minSpeed,
			    double maxSpeed, double targetTimeSeconds)
	: speed_(initialSpeed), speed2_(speed2), minSpeed_(minSpeed), maxSpeed_(maxSpeed),
	  targetTimeSeconds_(targetTimeSeconds) {}

double SpeedAdapter::recordCycleAndGetNextSpeed(double elapsedSeconds) {
	if (!firstCycleSeen_) {
		firstCycleSeen_ = true;
		return speed_;
	}

	samples_.push_back(elapsedSeconds);
	if (samples_.size() < kHistoryWindow)
		return speed_;

	const double avg =
		std::accumulate(samples_.begin(), samples_.end(), 0.0) / samples_.size();
	samples_.clear(); // start collecting a fresh, all-post-correction window

	const double error = (avg - targetTimeSeconds_) / targetTimeSeconds_;
	const double gamma = std::clamp(error, -kMaxSlowDownFraction, kMaxSpeedUpFraction);

	speed_ = std::clamp(speed_ * (1.0 + gamma), minSpeed_, maxSpeed_);
	return speed_;
}

} // namespace hqcore
