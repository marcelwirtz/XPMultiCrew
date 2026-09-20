#include "net/link_quality.h"

#include <algorithm>

namespace flytogether {

namespace {
// A handful of packets to converge, not hundreds - same order of magnitude
// as RemoteAircraft's own PD blend tuning, chosen for the same reason: this
// is a live health indicator someone might glance at mid-flight, not a
// long-run statistic.
constexpr double kEmaAlpha = 0.15;

// Caps an implausibly large sequence delta (e.g. the sender restarted and
// its sequence counter reset, wrapping around to a huge unsigned
// difference from our point of view) so one such event doesn't pin the
// estimate at ~100% loss - it's read as "one gap", not "thousands of lost
// packets".
constexpr uint32_t kMaxPlausibleDelta = 1000;
} // namespace

void LinkQualityTracker::OnPacketReceived(uint32_t sequence) {
    if (!has_previous_) {
        has_previous_ = true;
        previous_sequence_ = sequence;
        return;
    }

    // Wraparound-safe delta: unsigned subtraction already gives the right
    // small positive value both for a normal forward step and for a step
    // across a uint32_t wrap, same reasoning as every other "newer than"
    // check in this codebase.
    const uint32_t delta = sequence - previous_sequence_;
    previous_sequence_ = sequence;
    if (delta == 0) {
        return; // duplicate/retransmit, not a gap
    }

    const uint32_t clamped_delta = std::min(delta, kMaxPlausibleDelta);
    // delta == 1 means back-to-back (no loss); a larger delta means
    // (delta - 1) packets were lost between the last two we actually saw.
    const double sample_loss = static_cast<double>(clamped_delta - 1) / static_cast<double>(clamped_delta);
    ema_loss_ratio_ = kEmaAlpha * sample_loss + (1.0 - kEmaAlpha) * ema_loss_ratio_;
}

} // namespace flytogether
