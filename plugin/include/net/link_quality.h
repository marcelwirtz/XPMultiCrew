#pragma once

#include <cstdint>

namespace flytogether {

// Tracks a rolling estimate of packet loss for one incoming stream of
// AircraftStatePackets, purely from the gaps in their monotonically
// increasing (wrapping) `sequence` field - the same field
// RemoteAircraft::OnPacketReceived already uses to reject stale/duplicate
// packets, just fed here as well so link health becomes independently
// observable (see docs/plan.md's Verbindungsqualitäts-Anzeige feature).
//
// Deliberately not part of RemoteAircraft itself, whose own comment
// documents it as pure dead-reckoning math: this is a separate concern
// (network health) that both FormationSync (per peer) and
// SharedCockpitSync (the master link) attach as a sibling, fed at the same
// point they feed their own RemoteAircraft - see those classes' .cpp
// files.
//
// Pure math, no network/XPLM dependency - see tests/link_quality_test.cpp.
class LinkQualityTracker {
public:
    // Feed every accepted packet's sequence number, in receive order.
    void OnPacketReceived(uint32_t sequence);

    // Smoothed estimate of recent packet loss, 0.0 (none) .. 1.0 (all
    // lost) - an exponential moving average over consecutive gaps, not a
    // simple lifetime total, so a link that WAS bad but has since
    // recovered reflects that within a handful of packets instead of
    // being dragged down forever by one early outage. Meaningless before
    // HasData() is true (needs at least two packets to see a gap at all).
    double LossRatio() const { return ema_loss_ratio_; }

    bool HasData() const { return has_previous_; }

private:
    bool has_previous_ = false;
    uint32_t previous_sequence_ = 0;
    double ema_loss_ratio_ = 0.0;
};

} // namespace flytogether
