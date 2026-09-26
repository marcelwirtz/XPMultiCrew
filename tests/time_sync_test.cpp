// Pure-logic test for sync/time_sync.h - day/year wrap handling, the drift
// threshold, and rejecting garbage on decode.

#include "sync/time_sync.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>

using namespace flytogether;

namespace {

TimeSyncPacket At(int days, float seconds) {
    TimeSyncPacket p;
    p.local_date_days = days;
    p.zulu_time_sec = seconds;
    return p;
}

} // namespace

int main() {
    assert(std::fabs(TimeSyncDifferenceS(At(10, 3600.0f), At(10, 3700.0f)) - 100.0) < 1e-3);
    // Midnight wrap: 23:59 day 10 -> 00:01 day 11 is +120s.
    assert(std::fabs(TimeSyncDifferenceS(At(10, 86340.0f), At(11, 60.0f)) - 120.0) < 1e-3);
    // Year wrap: Dec 31 23:59 -> Jan 1 00:01 is +120s too.
    assert(std::fabs(TimeSyncDifferenceS(At(364, 86340.0f), At(0, 60.0f)) - 120.0) < 1e-3);
    assert(std::fabs(TimeSyncDifferenceS(At(0, 60.0f), At(364, 86340.0f)) + 120.0) < 1e-3);
    std::printf("TimeSyncDifferenceS handles day/year wrap: OK\n");

    assert(!TimeSyncShouldApply(At(10, 1000.0f), At(10, 1030.0f)));
    assert(TimeSyncShouldApply(At(10, 1000.0f), At(10, 1100.0f)));
    assert(TimeSyncShouldApply(At(10, 1000.0f), At(11, 1000.0f)));
    std::printf("TimeSyncShouldApply respects the drift threshold: OK\n");

    TimeSyncPacket good = At(100, 43200.0f);
    uint8_t bytes[sizeof(good)];
    std::memcpy(bytes, &good, sizeof(good));
    assert(DecodeTimeSyncPacket(bytes, sizeof(bytes)).has_value());
    assert(!DecodeTimeSyncPacket(bytes, sizeof(bytes) - 1).has_value());
    TimeSyncPacket bad = At(400, 10.0f);
    std::memcpy(bytes, &bad, sizeof(bad));
    assert(!DecodeTimeSyncPacket(bytes, sizeof(bytes)).has_value());
    bad = At(1, std::nanf(""));
    std::memcpy(bytes, &bad, sizeof(bad));
    assert(!DecodeTimeSyncPacket(bytes, sizeof(bytes)).has_value());
    std::printf("DecodeTimeSyncPacket rejects short/out-of-range packets: OK\n");

    std::printf("\nALL TIME SYNC CHECKS PASSED\n");
    return 0;
}
