#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

namespace flytogether {

// Time-of-day sync: one side (Formation: the session creator; Shared
// Cockpit: the master) periodically broadcasts its sim date/time, the
// others jump to it when they've drifted too far. Travels over the same
// encrypted relay/direct stream as the other session messages and is told
// apart by its magic.
constexpr uint32_t kTimeSyncMagic = 0x46545431; // "FTT1"
constexpr uint32_t kTimeSyncProtocolVersion = 1;

#pragma pack(push, 1)
struct TimeSyncPacket {
    uint32_t magic = kTimeSyncMagic;
    uint32_t protocol_version = kTimeSyncProtocolVersion;
    int32_t local_date_days = 0; // sim/time/local_date_days, 0 = Jan 1
    float zulu_time_sec = 0.0f;  // sim/time/zulu_time_sec
};
#pragma pack(pop)

constexpr size_t kTimeSyncPacketSize = 16;
static_assert(sizeof(TimeSyncPacket) == kTimeSyncPacketSize, "TimeSyncPacket's wire size changed");

// Seconds between two broadcasts, and how far a follower may drift before
// it jumps. The threshold keeps normal clock/pause jitter from constantly
// re-setting the time (every set is a visible lighting jump).
constexpr double kTimeSyncBroadcastIntervalS = 10.0;
constexpr double kTimeSyncMaxDriftS = 60.0;

std::optional<TimeSyncPacket> DecodeTimeSyncPacket(const uint8_t* data, size_t len);

// Signed difference remote - local in seconds, across day (and year) wrap:
// 23:59 on day 10 vs 00:01 on day 11 is +120s, not -86280s.
double TimeSyncDifferenceS(const TimeSyncPacket& local, const TimeSyncPacket& remote);

bool TimeSyncShouldApply(const TimeSyncPacket& local, const TimeSyncPacket& remote);

// Pure, no XPLM - the sim reads/writes live in plugin_main.cpp.

} // namespace flytogether
