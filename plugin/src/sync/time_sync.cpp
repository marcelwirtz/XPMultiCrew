#include "sync/time_sync.h"

#include <cmath>
#include <cstring>

namespace flytogether {

namespace {
constexpr double kSecondsPerDay = 86400.0;
constexpr int kDaysPerYear = 365; // X-Plane's local_date_days runs 0..364
} // namespace

std::optional<TimeSyncPacket> DecodeTimeSyncPacket(const uint8_t* data, size_t len) {
    if (len < kTimeSyncPacketSize) {
        return std::nullopt;
    }
    TimeSyncPacket packet;
    std::memcpy(&packet, data, sizeof(packet));
    if (packet.magic != kTimeSyncMagic || packet.protocol_version < 1 || !std::isfinite(packet.zulu_time_sec) ||
        packet.zulu_time_sec < 0.0f || packet.zulu_time_sec >= kSecondsPerDay || packet.local_date_days < 0 ||
        packet.local_date_days >= kDaysPerYear) {
        return std::nullopt;
    }
    return packet;
}

double TimeSyncDifferenceS(const TimeSyncPacket& local, const TimeSyncPacket& remote) {
    int day_diff = remote.local_date_days - local.local_date_days;
    // Shortest way around the year (Dec 31 -> Jan 1 is one day, not -364).
    if (day_diff > kDaysPerYear / 2) day_diff -= kDaysPerYear;
    if (day_diff < -kDaysPerYear / 2) day_diff += kDaysPerYear;
    return day_diff * kSecondsPerDay + (static_cast<double>(remote.zulu_time_sec) - local.zulu_time_sec);
}

bool TimeSyncShouldApply(const TimeSyncPacket& local, const TimeSyncPacket& remote) {
    return std::fabs(TimeSyncDifferenceS(local, remote)) > kTimeSyncMaxDriftS;
}

} // namespace flytogether
