#pragma once

#include "XPMPAircraft.h"
#include "sync/remote_aircraft.h"

#include <cstdint>

namespace flytogether {

// Bridges one tracked remote aircraft (network + dead reckoning, see
// sync/remote_aircraft.h) to XPMP2's rendering (TCAS override + XPLMInstance
// with a CSL model). The plugin pushes a fresh dead-reckoned pose once per
// frame via SetPose(); XPMP2 calls UpdatePosition() every frame it draws,
// which just applies whatever was last pushed. Decoupling it this way means
// this object owns no pointer into FormationSync's internal state, so their
// lifetimes don't need to be coordinated.
class RemoteAircraftXPMP : public XPMP2::Aircraft {
public:
    // `icaoType` may be empty/unrecognized - only one CSL model ships today
    // (plugin/Resources/CSL/Generic), matched via the "GENR" default ICAO
    // regardless of the real type, so this never fails to find a model.
    RemoteAircraftXPMP(const std::string& icaoType, uint32_t senderId);

    // `latest` supplies what isn't dead-reckoned: callsign and the extra
    // animation inputs (protocol_version 2 fields - defaults for older
    // senders, see aircraft_state.h).
    void SetPose(const AircraftPose& pose, const AircraftStatePacket& latest);

    // Re-matches the CSL model if the peer switched aircraft type
    // mid-session (no-op if unchanged). Matters as soon as a user has a
    // real CSL package installed next to the generic model.
    void UpdateIcaoType(const std::string& icaoType);

    void UpdatePosition(float elapsedSinceLastCall, int flCounter) override;

private:
    std::string requested_icao_;
    std::string applied_callsign_;
    AircraftPose pose_;
    AircraftStatePacket latest_;
    float tire_angle_deg_ = 0.0f;
    float prop_angle_deg_ = 0.0f;
};

} // namespace flytogether
