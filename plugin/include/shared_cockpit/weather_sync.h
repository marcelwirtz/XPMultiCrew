#pragma once

#include "flytogether/udp_socket.h"
#include "formation/peer_list.h"
#include "shared_cockpit/shared_cockpit_sync.h" // SharedCockpitRole
#include "shared_cockpit/weather_sync_protocol.h"

#include <functional>
#include <vector>

namespace flytogether {

// Shared Cockpit weather sync: master reads its own local weather
// periodically and broadcasts it; client applies whatever it last
// received directly via XPLMSetWeatherAtLocation - no smoothing/dead-
// reckoning the way position sync has (RemoteAircraft) - weather changes
// slowly, and X-Plane's own weather engine already blends toward newly
// set weather over its own transition window unless told to apply
// immediately (see the .cpp). Neither side calls into XPLMWeather.h every
// frame, by design: its own docs say those calls "should be called only
// during the pre-flight loop callback", i.e. occasionally, not every
// tick - MaybeBroadcast/PollIncoming below are both safe to call every
// frame like the other sync engines (the throttling happens internally),
// but only actually touch XPLMWeather.h at most once every
// kWeatherApplyIntervalS seconds, plus once immediately for the client's
// very first received packet (so a freshly-connected client doesn't wait
// out a full interval for its first real update).
//
// See weather_sync_protocol.h's file comment for why this isn't adapted
// from JoinFS (it has no X-Plane-side weather sync to adapt from).
//
// Reuses SharedCockpitRole (shared_cockpit_sync.h) rather than defining
// its own - this only makes sense paired with an active Shared Cockpit
// session, so it shares that session's role and peer list.
//
// XPLM-touching (XPLMWeather.h), like DatarefSync - not unit-tested
// directly for the same reason DatarefSync isn't (see that class's own
// comment); the wire format it reads/writes is
// (weather_sync_protocol.h, tests/weather_sync_protocol_test.cpp).
class WeatherSync {
public:
    bool Start(SharedCockpitRole role, const std::vector<Peer>& peers);
    // No socket of its own: packets only go out through the relay sender
    // and come in through IngestRelayedPacket. Used by Formation's time &
    // weather sync, which runs alongside Shared Cockpit's instance (and so
    // can't bind kWeatherSyncUdpPort a second time).
    void StartRelayOnly(SharedCockpitRole role);
    void Stop();

    // Changes who shares and who follows without restarting - Shared
    // Cockpit's role swap, Formation's host detection. Becoming a client
    // applies the next received packet immediately.
    void SetRole(SharedCockpitRole role);
    SharedCockpitRole role() const { return role_; }

    // Master only: reads local weather via XPLMGetWeatherAtLocation at
    // the given position and broadcasts it, throttled internally to
    // kWeatherApplyIntervalS. No-op unless role() == kMaster.
    void MaybeBroadcast(double latitude, double longitude, double altitude_m, double now_s);

    // Client only: drains incoming packets (cheap, every frame) and
    // applies the latest one via XPLMSetWeatherAtLocation at the given
    // (client-side, presumably moving) position - immediately for the
    // first packet ever received, otherwise throttled to
    // kWeatherApplyIntervalS (re-applying periodically even without a
    // new packet, since the client's own position may have drifted
    // outside the last report's radius of effect). No-op unless
    // role() == kClient.
    void PollIncoming(double latitude, double longitude, double ground_altitude_m, double now_s);

    void IngestRelayedPacket(const void* data, size_t len);

    void SetRelaySender(std::function<void(const void*, size_t)> sender) {
        relay_sender_ = std::move(sender);
    }

private:
    // Decodes into pending_packet_/has_pending_packet_ if valid; does not
    // touch XPLMWeather.h.
    void ApplyIncomingBytes(const void* data, size_t len);
    void ApplyPendingToSim(double latitude, double longitude, double ground_altitude_m);

    SharedCockpitRole role_ = SharedCockpitRole::kNone;
    UdpSocket socket_;
    std::vector<Peer> peers_;
    double next_broadcast_time_s_ = 0.0; // master
    double next_apply_time_s_ = 0.0;     // client
    bool has_pending_packet_ = false;
    bool has_applied_once_ = false;
    WeatherStatePacket pending_packet_;
    std::function<void(const void*, size_t)> relay_sender_;
};

} // namespace flytogether
