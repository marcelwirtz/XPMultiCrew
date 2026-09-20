// Pure-data regression test for the Shared Cockpit weather wire struct -
// no XPLM dependency (the XPLMWeatherInfo_t <-> WeatherStatePacket
// conversion lives in weather_sync.cpp, which does need XPLM and isn't
// unit-tested directly, same reasoning as DatarefSync). See
// weather_sync_protocol.h's file comment for why this isn't adapted from
// JoinFS (it has no X-Plane-side weather sync to adapt from).

#include "shared_cockpit/weather_sync_protocol.h"

#include <cassert>
#include <cstdio>
#include <cstring>

using namespace flytogether;

namespace {

void TestDefaultConstructionSetsMagicAndVersion() {
    WeatherStatePacket packet;
    assert(packet.magic == kWeatherStateMagic);
    assert(packet.protocol_version == kWeatherStateProtocolVersion);
    assert(packet.wind_layer_count == 0);
    assert(packet.cloud_layer_count == 0);
    std::printf("TestDefaultConstructionSetsMagicAndVersion: OK\n");
}

// Exercises the exact pattern weather_sync.cpp's ApplyIncomingBytes uses:
// build a packet, copy it to a raw byte buffer (as if sent over UDP), copy
// it back into a fresh packet, and confirm every field survives - a
// packed struct with no padding/alignment surprises is what makes the
// raw-memcpy wire approach (same as AircraftStatePacket, not
// DatarefSyncMessage's length-prefixed encoding) safe in the first place.
void TestRawByteRoundTrip() {
    WeatherStatePacket sent;
    sent.temperature_c = 15.5f;
    sent.dewpoint_c = 10.2f;
    sent.pressure_pa = 101325.0f;
    sent.pressure_sl_pa = 101325.0f;
    sent.visibility_m = 10000.0f;
    sent.precip_rate = 0.0f;

    sent.wind_layer_count = 2;
    sent.wind_layers[0] = WeatherWindLayer{0.0f, 5.0f, 270.0f, 8.0f, 10.0f, 0.1f};
    sent.wind_layers[1] = WeatherWindLayer{3000.0f, 15.0f, 250.0f, 20.0f, 5.0f, 0.2f};

    sent.cloud_layer_count = 1;
    sent.cloud_layers[0] = WeatherCloudLayer{1.0f, 0.5f, 5000.0f, 3000.0f};

    uint8_t buf[sizeof(WeatherStatePacket)];
    std::memcpy(buf, &sent, sizeof(sent));

    WeatherStatePacket received;
    std::memcpy(&received, buf, sizeof(received));

    assert(received.magic == sent.magic);
    assert(received.protocol_version == sent.protocol_version);
    assert(received.temperature_c == sent.temperature_c);
    assert(received.dewpoint_c == sent.dewpoint_c);
    assert(received.pressure_pa == sent.pressure_pa);
    assert(received.visibility_m == sent.visibility_m);
    assert(received.wind_layer_count == 2);
    assert(received.wind_layers[0].direction_deg == 270.0f);
    assert(received.wind_layers[1].speed_mps == 15.0f);
    assert(received.cloud_layer_count == 1);
    assert(received.cloud_layers[0].coverage == 0.5f);
    std::printf("TestRawByteRoundTrip: OK\n");
}

// A future field addition must extend kWeatherStateMinSize's *frozen*
// value, never redefine it as sizeof(WeatherStatePacket) (which would
// just track the growing struct and defeat forward compatibility, same
// as aircraft_state.h's kAircraftStateMinSize) - this only re-confirms
// today's value matches (the header's own static_assert is the real
// guard, this is just visible in the test suite's normal output too).
void TestMinSizeMatchesStructSize() {
    assert(sizeof(WeatherStatePacket) == kWeatherStateMinSize);
    std::printf("TestMinSizeMatchesStructSize: OK\n");
}

} // namespace

int main() {
    TestDefaultConstructionSetsMagicAndVersion();
    TestRawByteRoundTrip();
    TestMinSizeMatchesStructSize();
    std::printf("All weather_sync_protocol tests passed.\n");
    return 0;
}
