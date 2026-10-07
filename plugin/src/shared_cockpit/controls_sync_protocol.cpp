#include "shared_cockpit/controls_sync_protocol.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace flytogether {

namespace {

constexpr size_t kFixedFloats = 5;
constexpr size_t kHeaderSize = 4 + 1 + 1 + 2 + 4; // magic, version, engines, pad, sequence

void PutFloat(std::vector<uint8_t>& out, float value) {
    uint8_t bytes[4];
    std::memcpy(bytes, &value, 4);
    out.insert(out.end(), bytes, bytes + 4);
}

float GetFloat(const uint8_t* data, size_t& offset, float lo, float hi) {
    float value = 0.0f;
    std::memcpy(&value, data + offset, 4);
    offset += 4;
    if (!std::isfinite(value)) {
        return 0.0f;
    }
    return std::clamp(value, lo, hi);
}

} // namespace

std::vector<uint8_t> EncodeControlsState(const ControlsState& state) {
    const int engines = std::clamp(state.engines, 0, kControlsMaxEngines);
    std::vector<uint8_t> out;
    out.reserve(kHeaderSize + (kFixedFloats + 3 * engines) * 4);
    uint8_t header[kHeaderSize] = {};
    std::memcpy(header, &kControlsSyncMagic, 4);
    header[4] = kControlsSyncVersion;
    header[5] = static_cast<uint8_t>(engines);
    std::memcpy(header + 8, &state.sequence, 4);
    out.insert(out.end(), header, header + kHeaderSize);
    PutFloat(out, state.yoke_pitch);
    PutFloat(out, state.yoke_roll);
    PutFloat(out, state.yoke_heading);
    PutFloat(out, state.left_brake);
    PutFloat(out, state.right_brake);
    for (int i = 0; i < engines; ++i) {
        PutFloat(out, state.throttle[i]);
        PutFloat(out, state.mixture[i]);
        PutFloat(out, state.prop[i]);
    }
    return out;
}

std::optional<ControlsState> DecodeControlsState(const uint8_t* data, size_t len) {
    if (!data || len < kHeaderSize + kFixedFloats * 4) {
        return std::nullopt;
    }
    uint32_t magic = 0;
    std::memcpy(&magic, data, 4);
    if (magic != kControlsSyncMagic || data[4] != kControlsSyncVersion || data[5] > kControlsMaxEngines) {
        return std::nullopt;
    }
    ControlsState state;
    state.engines = data[5];
    if (len < kHeaderSize + (kFixedFloats + 3 * static_cast<size_t>(state.engines)) * 4) {
        return std::nullopt;
    }
    std::memcpy(&state.sequence, data + 8, 4);
    size_t offset = kHeaderSize;
    state.yoke_pitch = GetFloat(data, offset, -1.0f, 1.0f);
    state.yoke_roll = GetFloat(data, offset, -1.0f, 1.0f);
    state.yoke_heading = GetFloat(data, offset, -1.0f, 1.0f);
    state.left_brake = GetFloat(data, offset, 0.0f, 1.0f);
    state.right_brake = GetFloat(data, offset, 0.0f, 1.0f);
    for (int i = 0; i < state.engines; ++i) {
        state.throttle[i] = GetFloat(data, offset, -2.0f, 1.5f);
        state.mixture[i] = GetFloat(data, offset, 0.0f, 1.0f);
        state.prop[i] = GetFloat(data, offset, 0.0f, 1.0f);
    }
    return state;
}

} // namespace flytogether
