#pragma once

#include <cstddef>
#include <cstdint>

// Wire format for Shared Cockpit weather sync - master reads its own local
// weather (XPLMGetWeatherAtLocation) and broadcasts it periodically; the
// client applies whatever it last received (XPLMSetWeatherAtLocation) -
// see weather_sync.h. Not built on JoinFS: grepping JoinFS-XP/JoinFS-XP.cpp
// and JoinFS/XPlane.cs for weather/METAR found zero hits - JoinFS's own
// weather sync (WeatherRequest/Reply/Update) is a SimConnect/MSFS-only
// feature that was never ported to its X-Plane build, so there's no
// reference implementation to adapt. This is built directly on X-Plane
// 12's own XPLMWeather.h API instead (XPLM420+, i.e. X-Plane 12.1+).
//
// A fixed #pragma-packed struct sent raw (same approach as
// flytogether/aircraft_state.h's AircraftStatePacket, not
// dataref_sync_protocol.h's length-prefixed encoding - weather has a
// fixed shape, there's no variable-length name to carry). Carries a
// subset of XPLMWeatherInfo_t: temperature/dewpoint/pressure/visibility/
// precip plus the wind and cloud layers - the fields that actually matter
// for two pilots' local conditions looking and feeling consistent
// (wind especially). Deliberately not carrying temp_layers/dewp_layers/
// troposphere_* (XPLMWeatherInfo_t's atmosphere-profile fields, mostly
// relevant to high-altitude performance modeling) - can be added later
// following AircraftStatePacket's tail-append/version-gated-read
// discipline (see aircraft_state.h's kAircraftStateMinSize comment) if
// ever needed.
//
// Pure data, no XPLM dependency - see tests/weather_sync_protocol_test.cpp.
// The actual XPLMWeatherInfo_t <-> WeatherStatePacket conversion and the
// XPLM calls live in shared_cockpit/weather_sync.h, which does need XPLM.

namespace flytogether {

// Was "FTS5" up to v0.2.x - the same value as kOwnershipClaimMagic, which
// only worked because one travels encrypted and the other in plaintext.
constexpr uint32_t kWeatherStateMagic = 0x46545731; // "FTW1"

// Same versioning split as aircraft_state.h's
// kAircraftStateProtocolVersion/kAircraftStateMinProtocolVersion pair -
// see that file's comment for why they're deliberately two separate
// constants rather than one.
constexpr uint32_t kWeatherStateProtocolVersion = 1;
constexpr uint32_t kWeatherStateMinProtocolVersion = 1;

constexpr uint16_t kWeatherSyncUdpPort = 49022;

// XPLM_NUM_WIND_LAYERS / XPLM_NUM_CLOUD_LAYERS as of XPLMWeather.h's
// current version - not read from the SDK header directly since this
// struct's layout must stay independent of it (see the file comment).
constexpr int kWeatherMaxWindLayers = 13;
constexpr int kWeatherMaxCloudLayers = 3;

#pragma pack(push, 1)
struct WeatherWindLayer {
    float alt_msl_m = 0.0f;
    float speed_mps = -1.0f; // negative = undefined, matches XPLM's own convention
    float direction_deg = 0.0f;
    float gust_speed_mps = 0.0f;
    float shear_deg = 0.0f;
    float turbulence = 0.0f;
};

struct WeatherCloudLayer {
    float cloud_type = 0.0f;
    float coverage = 0.0f;
    float alt_top_m = 0.0f;
    float alt_base_m = 0.0f;
};

struct WeatherStatePacket {
    uint32_t magic = kWeatherStateMagic;
    uint32_t protocol_version = kWeatherStateProtocolVersion;

    float temperature_c = 0.0f;
    float dewpoint_c = 0.0f;
    float pressure_pa = 0.0f;    // QNH at the reporting location; 0 = use pressure_sl_pa instead
    float pressure_sl_pa = 0.0f; // sea-level pressure, used when pressure_pa is 0
    float visibility_m = 0.0f;
    float precip_rate = 0.0f;

    uint8_t wind_layer_count = 0; // <= kWeatherMaxWindLayers
    WeatherWindLayer wind_layers[kWeatherMaxWindLayers];

    uint8_t cloud_layer_count = 0; // <= kWeatherMaxCloudLayers
    WeatherCloudLayer cloud_layers[kWeatherMaxCloudLayers];
};
#pragma pack(pop)

// Frozen baseline size - a literal, not sizeof(WeatherStatePacket), for
// the same reason aircraft_state.h's kAircraftStateMinSize is: so it
// can't silently track the struct's size once it grows. Verified against
// the actual struct by the static_assert below.
constexpr size_t kWeatherStateMinSize = 394;

static_assert(sizeof(WeatherStatePacket) == kWeatherStateMinSize,
              "WeatherStatePacket's size changed - see kWeatherStateMinSize's comment");

} // namespace flytogether
