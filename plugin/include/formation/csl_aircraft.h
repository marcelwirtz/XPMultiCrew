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

    void SetPose(const AircraftPose& pose);

    void UpdatePosition(float elapsedSinceLastCall, int flCounter) override;

private:
    AircraftPose pose_;
};

} // namespace flytogether
