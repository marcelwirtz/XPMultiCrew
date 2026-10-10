#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace flytogether {

// Shared Cockpit: the pilot flying's engine gauges, sent to the co-pilot 5
// times a second. X-Plane doesn't run the co-pilot's engine model while its
// position is overridden (live-tested: RPM, EGT, oil, fuel flow all froze),
// so without this the co-pilot's tach, EIS and fuel gauges would stand
// still. Aircraft-independent - no profile needed. Applied by
// plugin_main.cpp's ApplyRemoteEngines. Sealed like the other Shared
// Cockpit messages.
constexpr uint32_t kEngineSyncMagic = 0x4654454E; // "FTEN"
constexpr uint8_t kEngineSyncVersion = 1;
constexpr int kEngineSyncMaxEngines = 8;
constexpr int kEngineSyncMaxTanks = 9;

struct EngineGauges {
    float prop_rad_s = 0.0f;   // sim/flightmodel/engine/POINT_tacrad
    float engine_rad_s = 0.0f; // sim/flightmodel/engine/ENGN_tacrad
    float n1_percent = 0.0f;   // ENGN_N1_
    float n2_percent = 0.0f;   // ENGN_N2_
    float egt_c = 0.0f;        // sim/flightmodel2/engines/EGT_deg_cel
    float cht_c = 0.0f;        // sim/flightmodel2/engines/CHT_deg_cel
    float itt_c = 0.0f;        // sim/flightmodel2/engines/ITT_deg_cel
    float oil_temp = 0.0f;     // ENGN_oil_temp_c (units as the aircraft has them)
    float oil_press_psi = 0.0f;
    float fuel_flow_kg_s = 0.0f;
    float manifold_inhg = 0.0f; // ENGN_MPR
    float torque_nm = 0.0f;     // ENGN_TRQ
};

struct EngineState {
    uint32_t sequence = 0;
    int engines = 0; // valid entries, 0..kEngineSyncMaxEngines
    EngineGauges engine[kEngineSyncMaxEngines];
    int tanks = 0; // valid entries, 0..kEngineSyncMaxTanks
    float fuel_kg[kEngineSyncMaxTanks] = {}; // sim/flightmodel/weight/m_fuel
};

std::vector<uint8_t> EncodeEngineState(const EngineState& state);
// nullopt for anything that isn't a well-formed message; non-finite values
// become 0 and everything is clamped to a sane range (it comes from the
// network).
std::optional<EngineState> DecodeEngineState(const uint8_t* data, size_t len);

} // namespace flytogether
