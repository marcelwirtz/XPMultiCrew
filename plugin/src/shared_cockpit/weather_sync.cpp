#include "shared_cockpit/weather_sync.h"

#include "XPLMWeather.h"

#include <algorithm>
#include <cstring>

namespace flytogether {

namespace {

// Weather changes slowly - this interval satisfies both XPLMWeather.h's
// own "not intended to be used per-frame" guidance and the fact that
// there's nothing to gain from syncing it any faster.
constexpr double kWeatherApplyIntervalS = 30.0;

// Matches XPLM_DEFAULT_WXR_RADIUS_NM/XPLM_DEFAULT_WXR_LIMIT_MSL_FT
// (XPLMWeather.h) - not read from the SDK header directly (same reasoning
// as weather_sync_protocol.h's kWeatherMaxWindLayers/kWeatherMaxCloudLayers),
// used to fill the XPLMWeatherInfo_t fields our own wire packet doesn't
// carry (radius_nm/max_altitude_msl_ft) with X-Plane's own documented
// defaults rather than leaving them at 0 - a 0 radius/ceiling would mean
// "no effect at all", not "use the default effect area".
constexpr float kDefaultRadiusNm = 30.0f;
constexpr float kDefaultLimitFt = 10000.0f;

WeatherStatePacket ReadFromSim(double latitude, double longitude, double altitude_m) {
    XPLMWeatherInfo_t info{};
    info.structSize = sizeof(info);
    XPLMGetWeatherAtLocation(latitude, longitude, altitude_m, &info);

    WeatherStatePacket packet;
    packet.temperature_c = info.temperature_alt;
    packet.dewpoint_c = info.dewpoint_alt;
    packet.pressure_pa = info.pressure_alt;
    packet.pressure_sl_pa = info.pressure_sl;
    packet.visibility_m = info.visibility;
    packet.precip_rate = info.precip_rate;

    packet.wind_layer_count = static_cast<uint8_t>(std::min<int>(kWeatherMaxWindLayers, XPLM_NUM_WIND_LAYERS));
    for (int i = 0; i < packet.wind_layer_count; ++i) {
        const auto& src = info.wind_layers[i];
        auto& dst = packet.wind_layers[i];
        dst.alt_msl_m = src.alt_msl;
        dst.speed_mps = src.speed;
        dst.direction_deg = src.direction;
        dst.gust_speed_mps = src.gust_speed;
        dst.shear_deg = src.shear;
        dst.turbulence = src.turbulence;
    }

    packet.cloud_layer_count = static_cast<uint8_t>(std::min<int>(kWeatherMaxCloudLayers, XPLM_NUM_CLOUD_LAYERS));
    for (int i = 0; i < packet.cloud_layer_count; ++i) {
        const auto& src = info.cloud_layers[i];
        auto& dst = packet.cloud_layers[i];
        dst.cloud_type = src.cloud_type;
        dst.coverage = src.coverage;
        dst.alt_top_m = src.alt_top;
        dst.alt_base_m = src.alt_base;
    }

    return packet;
}

void ApplyToSim(const WeatherStatePacket& packet, double latitude, double longitude,
                 double ground_altitude_m) {
    XPLMWeatherInfo_t info{};
    info.structSize = sizeof(info);
    info.temperature_alt = packet.temperature_c;
    info.dewpoint_alt = packet.dewpoint_c;
    info.pressure_alt = packet.pressure_pa;
    info.pressure_sl = packet.pressure_sl_pa;
    info.visibility = packet.visibility_m;
    // precip_rate_alt/wind_dir_alt/wind_spd_alt/turbulence_alt/wave_length/
    // wave_speed are documented "unused when setting" (XPLMWeather.h) -
    // left at their zero-initialized defaults deliberately.
    info.radius_nm = kDefaultRadiusNm;
    info.max_altitude_msl_ft = kDefaultLimitFt;
    // age = 0 (already zero-initialized): this is a freshly-received
    // report, so it should carry full weight - see XPLMWeatherInfo_t's
    // own doc comment on `age`.

    const int wind_count = std::min<int>(packet.wind_layer_count, XPLM_NUM_WIND_LAYERS);
    for (int i = 0; i < wind_count; ++i) {
        const auto& src = packet.wind_layers[i];
        auto& dst = info.wind_layers[i];
        dst.alt_msl = src.alt_msl_m;
        dst.speed = src.speed_mps;
        dst.direction = src.direction_deg;
        dst.gust_speed = src.gust_speed_mps;
        dst.shear = src.shear_deg;
        dst.turbulence = src.turbulence;
    }
    // Any remaining layer slots beyond wind_count are left zeroed
    // (info{} above) - XPLM_WIND_UNDEFINED_LAYER-style "no data here"
    // would be more precise, but a zeroed layer (speed 0, undefined
    // marked via the -1 default our own struct's speed_mps carries when
    // the sender didn't populate it) is an acceptable v1 simplification.

    const int cloud_count = std::min<int>(packet.cloud_layer_count, XPLM_NUM_CLOUD_LAYERS);
    for (int i = 0; i < cloud_count; ++i) {
        const auto& src = packet.cloud_layers[i];
        auto& dst = info.cloud_layers[i];
        dst.cloud_type = src.cloud_type;
        dst.coverage = src.coverage;
        dst.alt_top = src.alt_top_m;
        dst.alt_base = src.alt_base_m;
    }

    XPLMBeginWeatherUpdate();
    // isIncremental=false: this replaces any weather record this plugin
    // previously set at this point, rather than accumulating stale ones.
    // updateImmediately=true: matches the "sync" intent - reflect the
    // master's weather now, not gradually over X-Plane's own 1-2 minute
    // transition window.
    XPLMSetWeatherAtLocation(latitude, longitude, ground_altitude_m, &info);
    XPLMEndWeatherUpdate(/*isIncremental=*/0, /*updateImmediately=*/1);
}

} // namespace

bool WeatherSync::Start(SharedCockpitRole role, const std::vector<Peer>& peers) {
    role_ = role;
    peers_ = peers;
    next_broadcast_time_s_ = 0.0;
    next_apply_time_s_ = 0.0;
    has_pending_packet_ = false;
    has_applied_once_ = false;

    if (!socket_.Open()) {
        return false;
    }
    if (!socket_.Bind(kWeatherSyncUdpPort)) {
        // Closed rather than left open-but-unbound: that socket is still in
        // blocking mode, so a later ReceiveFrom() would hang the sim.
        socket_.Close();
        return false;
    }
    socket_.SetNonBlocking(true);
    return true;
}

void WeatherSync::StartRelayOnly(SharedCockpitRole role) {
    socket_.Close();
    peers_.clear();
    role_ = SharedCockpitRole::kNone;
    SetRole(role);
}

void WeatherSync::SetRole(SharedCockpitRole role) {
    if (role == role_) {
        return;
    }
    role_ = role;
    next_broadcast_time_s_ = 0.0;
    next_apply_time_s_ = 0.0;
    has_pending_packet_ = false;
    has_applied_once_ = false;
}

void WeatherSync::Stop() {
    socket_.Close();
    role_ = SharedCockpitRole::kNone;
    peers_.clear();
    has_pending_packet_ = false;
    has_applied_once_ = false;
}

void WeatherSync::MaybeBroadcast(double latitude, double longitude, double altitude_m, double now_s) {
    if (role_ != SharedCockpitRole::kMaster) {
        return;
    }
    if (now_s < next_broadcast_time_s_) {
        return;
    }
    next_broadcast_time_s_ = now_s + kWeatherApplyIntervalS;

    const WeatherStatePacket packet = ReadFromSim(latitude, longitude, altitude_m);
    for (const auto& peer : peers_) {
        socket_.SendTo(peer.host, peer.port, &packet, sizeof(packet));
    }
    if (relay_sender_) {
        relay_sender_(&packet, sizeof(packet));
    }
}

void WeatherSync::PollIncoming(double latitude, double longitude, double ground_altitude_m, double now_s) {
    if (role_ != SharedCockpitRole::kClient) {
        return;
    }

    uint8_t buf[sizeof(WeatherStatePacket) + 64];
    while (true) {
        const int received = socket_.ReceiveFrom(buf, sizeof(buf));
        if (received < 0) {
            break;
        }
        ApplyIncomingBytes(buf, static_cast<size_t>(received));
    }

    if (!has_pending_packet_) {
        return;
    }
    if (has_applied_once_ && now_s < next_apply_time_s_) {
        return;
    }

    ApplyPendingToSim(latitude, longitude, ground_altitude_m);
    has_applied_once_ = true;
    next_apply_time_s_ = now_s + kWeatherApplyIntervalS;
}

void WeatherSync::IngestRelayedPacket(const void* data, size_t len) {
    if (role_ != SharedCockpitRole::kClient) {
        return;
    }
    ApplyIncomingBytes(data, len);
}

void WeatherSync::ApplyIncomingBytes(const void* data, size_t len) {
    if (len < kWeatherStateMinSize) {
        return; // truncated/malformed/foreign packet
    }
    WeatherStatePacket packet;
    std::memcpy(&packet, data, std::min(len, sizeof(packet)));
    if (packet.magic != kWeatherStateMagic || packet.protocol_version < kWeatherStateMinProtocolVersion) {
        return;
    }
    // Peer-supplied layer counts, clamped defensively - same reasoning as
    // DatarefSync's array_len clamp in ApplyValue.
    packet.wind_layer_count = static_cast<uint8_t>(std::min<int>(packet.wind_layer_count, kWeatherMaxWindLayers));
    packet.cloud_layer_count =
        static_cast<uint8_t>(std::min<int>(packet.cloud_layer_count, kWeatherMaxCloudLayers));

    pending_packet_ = packet;
    has_pending_packet_ = true;
}

void WeatherSync::ApplyPendingToSim(double latitude, double longitude, double ground_altitude_m) {
    ApplyToSim(pending_packet_, latitude, longitude, ground_altitude_m);
}

} // namespace flytogether
