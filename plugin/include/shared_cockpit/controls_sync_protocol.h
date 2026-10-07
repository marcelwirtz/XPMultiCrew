#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace flytogether {

// Shared Cockpit: the pilot flying's control inputs - yoke, rudder, toe
// brakes and the power levers - sent to the co-pilot 20 times a second, so
// the co-pilot's yoke and levers move with them. The co-pilot's own
// joystick and throttle quadrant are overridden meanwhile (see
// plugin_main.cpp's ApplyRemoteControls); otherwise both cockpits'
// hardware would overwrite each other. Sealed like the other Shared
// Cockpit messages.
constexpr uint32_t kControlsSyncMagic = 0x46544354; // "FTCT"
constexpr uint8_t kControlsSyncVersion = 1;
constexpr int kControlsMaxEngines = 4;

struct ControlsState {
    uint32_t sequence = 0;
    float yoke_pitch = 0.0f;   // -1..1
    float yoke_roll = 0.0f;    // -1..1
    float yoke_heading = 0.0f; // rudder pedals, -1..1
    float left_brake = 0.0f;   // toe brakes, 0..1
    float right_brake = 0.0f;
    int engines = 0; // valid entries below, 0..kControlsMaxEngines
    // throttle_beta_rev_ratio: 0..1 like throttle_ratio, below 0 in beta/reverse
    float throttle[kControlsMaxEngines] = {};
    float mixture[kControlsMaxEngines] = {};
    float prop[kControlsMaxEngines] = {};
};

std::vector<uint8_t> EncodeControlsState(const ControlsState& state);
// nullopt for anything that isn't a well-formed message; values are
// clamped to their ranges (it comes from the network).
std::optional<ControlsState> DecodeControlsState(const uint8_t* data, size_t len);

} // namespace flytogether
