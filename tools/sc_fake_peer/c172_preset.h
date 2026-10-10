#pragma once

// The bundled C172 Shared Cockpit profile
// (plugin/Resources/shared_cockpit_profiles/C172.txt) with the X-Plane type
// of each dataref and a label, for the GUI's cockpit panel. Only datarefs in
// the profile are applied by the plugin, so keep this in step with it.

#include "shared_cockpit/dataref_sync_protocol.h"

namespace sc_fake {

using flytogether::DatarefValueType;

// kind: "switch" (0/1, arrays: element 0), "number", "ratio" (0..1 or
// -1..1), "output" (simulation result, the pilot flying's only).
struct PresetDataref {
    const char* name;
    DatarefValueType type;
    const char* group;
    const char* label;
    const char* kind;
};

constexpr DatarefValueType I = DatarefValueType::kInt;
constexpr DatarefValueType F = DatarefValueType::kFloat;
constexpr DatarefValueType IA = DatarefValueType::kIntArray;
constexpr DatarefValueType FA = DatarefValueType::kFloatArray;

inline constexpr PresetDataref kC172Datarefs[] = {
    {"sim/cockpit2/electrical/battery_on", IA, "Electrical", "Battery", "switch"},
    {"sim/cockpit2/electrical/generator_on", IA, "Electrical", "Alternator", "switch"},
    {"sim/cockpit2/switches/avionics_power_on", I, "Electrical", "Avionics master", "switch"},
    {"sim/cockpit/switches/pitot_heat_on", I, "Electrical", "Pitot heat", "switch"},
    {"sim/cockpit/electrical/beacon_lights_on", I, "Lights", "Beacon", "switch"},
    {"sim/cockpit/electrical/nav_lights_on", I, "Lights", "Nav lights", "switch"},
    {"sim/cockpit/electrical/strobe_lights_on", I, "Lights", "Strobes", "switch"},
    {"sim/cockpit/electrical/landing_lights_on", I, "Lights", "Landing light", "switch"},
    {"sim/cockpit/engine/fuel_pump_on", IA, "Engine", "Fuel pump", "switch"},
    {"sim/cockpit2/engine/actuators/primer_on", IA, "Engine", "Primer", "switch"},
    {"sim/cockpit2/engine/actuators/carb_heat_ratio", FA, "Engine", "Carb heat (0..1)", "ratio"},
    {"sim/cockpit2/engine/actuators/ignition_key", IA, "Engine", "Ignition key (0 off 1 R 2 L 3 both 4 start)", "output"},
    {"sim/flightmodel/engine/ENGN_running", IA, "Engine", "Engine running", "output"},
    {"sim/flightmodel/controls/flaprqst", F, "Controls", "Flaps (0 / .33 / .67 / 1)", "ratio"},
    {"sim/flightmodel/controls/parkbrake", F, "Controls", "Parking brake (0..1)", "ratio"},
    {"sim/cockpit2/controls/elevator_trim", F, "Controls", "Elevator trim (-1..1)", "ratio"},
    {"sim/cockpit2/controls/aileron_trim", F, "Controls", "Aileron trim (-1..1)", "ratio"},
    {"sim/cockpit2/controls/rudder_trim", F, "Controls", "Rudder trim (-1..1)", "ratio"},
    {"sim/cockpit2/switches/alternate_static_air_ratio", F, "Controls", "Alternate static (0..1)", "ratio"},
    {"sim/cockpit2/radios/actuators/com1_frequency_hz", I, "Radios", "COM1 (x10 kHz)", "number"},
    {"sim/cockpit2/radios/actuators/com1_standby_frequency_hz", I, "Radios", "COM1 standby", "number"},
    {"sim/cockpit2/radios/actuators/com2_frequency_hz", I, "Radios", "COM2", "number"},
    {"sim/cockpit2/radios/actuators/com2_standby_frequency_hz", I, "Radios", "COM2 standby", "number"},
    {"sim/cockpit2/radios/actuators/nav1_frequency_hz", I, "Radios", "NAV1", "number"},
    {"sim/cockpit2/radios/actuators/nav1_standby_frequency_hz", I, "Radios", "NAV1 standby", "number"},
    {"sim/cockpit2/radios/actuators/nav2_frequency_hz", I, "Radios", "NAV2", "number"},
    {"sim/cockpit2/radios/actuators/nav2_standby_frequency_hz", I, "Radios", "NAV2 standby", "number"},
    {"sim/cockpit/radios/adf1_freq_hz", I, "Radios", "ADF", "number"},
    {"sim/cockpit/radios/adf1_stdby_freq_hz", I, "Radios", "ADF standby", "number"},
    {"sim/cockpit/radios/dme_freq_hz", I, "Radios", "DME", "number"},
    {"sim/cockpit2/radios/actuators/nav1_obs_deg_mag_pilot", F, "Radios", "NAV1 OBS", "number"},
    {"sim/cockpit2/radios/actuators/nav2_obs_deg_mag_pilot", F, "Radios", "NAV2 OBS", "number"},
    {"sim/cockpit2/radios/actuators/audio_com_selection", I, "Radios", "Audio COM select", "number"},
    {"sim/cockpit2/radios/actuators/audio_nav_selection", I, "Radios", "Audio NAV select", "number"},
    {"sim/cockpit/radios/transponder_code", I, "Transponder", "Squawk", "number"},
    {"sim/cockpit/radios/transponder_mode", I, "Transponder", "Mode", "number"},
    {"sim/cockpit2/autopilot/heading_dial_deg_mag_pilot", F, "Autopilot", "Heading bug", "number"},
    {"sim/cockpit2/autopilot/altitude_dial_ft", F, "Autopilot", "Altitude (ft)", "number"},
    {"sim/cockpit2/autopilot/vvi_dial_fpm", F, "Autopilot", "Vertical speed (fpm)", "number"},
    {"sim/cockpit2/autopilot/airspeed_dial_kts", F, "Autopilot", "Airspeed (kt)", "number"},
};

struct PresetCommand {
    const char* name;
    const char* group;
    const char* label;
};

inline constexpr PresetCommand kC172Commands[] = {
    {"laminar/c172/ignition_up", "Engine", "Key right"},
    {"laminar/c172/ignition_down", "Engine", "Key left"},
    {"laminar/c172/fuel_selector_up", "Engine", "Fuel sel. right"},
    {"laminar/c172/fuel_selector_dwn", "Engine", "Fuel sel. left"},
    {"sim/starters/shut_down", "Engine", "Shut down"},
    {"sim/autopilot/servos_toggle", "Autopilot", "AP"},
    {"sim/autopilot/fdir_toggle", "Autopilot", "FD"},
    {"sim/autopilot/heading", "Autopilot", "HDG"},
    {"sim/autopilot/NAV", "Autopilot", "NAV"},
    {"sim/autopilot/approach", "Autopilot", "APR"},
    {"sim/autopilot/back_course", "Autopilot", "BC"},
    {"sim/autopilot/altitude_hold", "Autopilot", "ALT"},
    {"sim/autopilot/vertical_speed", "Autopilot", "VS"},
    {"sim/autopilot/level_change", "Autopilot", "FLC"},
    {"sim/autopilot/nose_up", "Autopilot", "Nose up"},
    {"sim/autopilot/nose_down", "Autopilot", "Nose down"},
    {"sim/autopilot/heading_sync", "Autopilot", "HDG sync"},
    {"sim/transponder/transponder_ident", "Transponder", "IDENT"},
    {"sim/GPS/g1000n1_direct", "G1000", "PFD Direct"},
    {"sim/GPS/g1000n1_fpl", "G1000", "PFD FPL"},
    {"sim/GPS/g1000n1_proc", "G1000", "PFD PROC"},
    {"sim/GPS/g1000n1_clr", "G1000", "PFD CLR"},
    {"sim/GPS/g1000n1_ent", "G1000", "PFD ENT"},
    {"sim/GPS/g1000n1_menu", "G1000", "PFD MENU"},
    {"sim/GPS/g1000n3_direct", "G1000", "MFD Direct"},
    {"sim/GPS/g1000n3_fpl", "G1000", "MFD FPL"},
};

// One-click cockpit states: dataref name -> value (arrays: element 0, the
// rest kept). Sent as ordinary switch changes, so as co-pilot they're
// requests the pilot flying applies.
struct PresetSetting {
    const char* name;
    double value;
};

struct PresetScenario {
    const char* label;
    const PresetSetting* settings;
    int count;
};

inline constexpr PresetSetting kColdDark[] = {
    {"sim/cockpit2/electrical/battery_on", 0},       {"sim/cockpit2/electrical/generator_on", 0},
    {"sim/cockpit2/switches/avionics_power_on", 0}, {"sim/cockpit/switches/pitot_heat_on", 0},
    {"sim/cockpit/electrical/beacon_lights_on", 0}, {"sim/cockpit/electrical/nav_lights_on", 0},
    {"sim/cockpit/electrical/strobe_lights_on", 0}, {"sim/cockpit/electrical/landing_lights_on", 0},
    {"sim/cockpit/engine/fuel_pump_on", 0},          {"sim/flightmodel/controls/flaprqst", 0},
    {"sim/flightmodel/controls/parkbrake", 1},
};

inline constexpr PresetSetting kBeforeStart[] = {
    {"sim/cockpit2/electrical/battery_on", 1},       {"sim/cockpit2/electrical/generator_on", 1},
    {"sim/cockpit2/switches/avionics_power_on", 0}, {"sim/cockpit/electrical/beacon_lights_on", 1},
    {"sim/cockpit/electrical/nav_lights_on", 1},     {"sim/cockpit/engine/fuel_pump_on", 1},
    {"sim/flightmodel/controls/parkbrake", 1},
};

inline constexpr PresetSetting kReadyForTakeoff[] = {
    {"sim/cockpit2/electrical/battery_on", 1},        {"sim/cockpit2/electrical/generator_on", 1},
    {"sim/cockpit2/switches/avionics_power_on", 1},  {"sim/cockpit/electrical/beacon_lights_on", 1},
    {"sim/cockpit/electrical/nav_lights_on", 1},      {"sim/cockpit/electrical/strobe_lights_on", 1},
    {"sim/cockpit/electrical/landing_lights_on", 1}, {"sim/cockpit/engine/fuel_pump_on", 1},
    {"sim/flightmodel/controls/flaprqst", 0},         {"sim/cockpit2/controls/elevator_trim", 0},
    {"sim/flightmodel/controls/parkbrake", 0},        {"sim/cockpit/radios/transponder_code", 7000},
};

inline constexpr PresetScenario kC172Scenarios[] = {
    {"Cold & dark", kColdDark, static_cast<int>(sizeof(kColdDark) / sizeof(kColdDark[0]))},
    {"Before start", kBeforeStart, static_cast<int>(sizeof(kBeforeStart) / sizeof(kBeforeStart[0]))},
    {"Ready for takeoff", kReadyForTakeoff, static_cast<int>(sizeof(kReadyForTakeoff) / sizeof(kReadyForTakeoff[0]))},
};

} // namespace sc_fake
