#include "shared_cockpit/engine_sync_protocol.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace flytogether {

namespace {

constexpr size_t kHeaderSize = 4 + 1 + 1 + 1 + 1 + 4; // magic, version, engines, tanks, pad, sequence
constexpr size_t kFloatsPerEngine = 12;

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

std::vector<uint8_t> EncodeEngineState(const EngineState& state) {
    const int engines = std::clamp(state.engines, 0, kEngineSyncMaxEngines);
    const int tanks = std::clamp(state.tanks, 0, kEngineSyncMaxTanks);
    std::vector<uint8_t> out;
    out.reserve(kHeaderSize + (kFloatsPerEngine * engines + tanks) * 4);
    uint8_t header[kHeaderSize] = {};
    std::memcpy(header, &kEngineSyncMagic, 4);
    header[4] = kEngineSyncVersion;
    header[5] = static_cast<uint8_t>(engines);
    header[6] = static_cast<uint8_t>(tanks);
    std::memcpy(header + 8, &state.sequence, 4);
    out.insert(out.end(), header, header + kHeaderSize);
    for (int i = 0; i < engines; ++i) {
        const EngineGauges& e = state.engine[i];
        for (float v : {e.prop_rad_s, e.engine_rad_s, e.n1_percent, e.n2_percent, e.egt_c, e.cht_c, e.itt_c,
                        e.oil_temp, e.oil_press_psi, e.fuel_flow_kg_s, e.manifold_inhg, e.torque_nm}) {
            PutFloat(out, v);
        }
    }
    for (int i = 0; i < tanks; ++i) {
        PutFloat(out, state.fuel_kg[i]);
    }
    return out;
}

std::optional<EngineState> DecodeEngineState(const uint8_t* data, size_t len) {
    if (!data || len < kHeaderSize) {
        return std::nullopt;
    }
    uint32_t magic = 0;
    std::memcpy(&magic, data, 4);
    if (magic != kEngineSyncMagic || data[4] != kEngineSyncVersion || data[5] > kEngineSyncMaxEngines ||
        data[6] > kEngineSyncMaxTanks) {
        return std::nullopt;
    }
    EngineState state;
    state.engines = data[5];
    state.tanks = data[6];
    if (len < kHeaderSize + (kFloatsPerEngine * state.engines + state.tanks) * 4) {
        return std::nullopt;
    }
    std::memcpy(&state.sequence, data + 8, 4);
    size_t offset = kHeaderSize;
    for (int i = 0; i < state.engines; ++i) {
        EngineGauges& e = state.engine[i];
        e.prop_rad_s = GetFloat(data, offset, 0.0f, 2000.0f);
        e.engine_rad_s = GetFloat(data, offset, 0.0f, 5000.0f);
        e.n1_percent = GetFloat(data, offset, 0.0f, 150.0f);
        e.n2_percent = GetFloat(data, offset, 0.0f, 150.0f);
        e.egt_c = GetFloat(data, offset, -100.0f, 2000.0f);
        e.cht_c = GetFloat(data, offset, -100.0f, 1000.0f);
        e.itt_c = GetFloat(data, offset, -100.0f, 2000.0f);
        e.oil_temp = GetFloat(data, offset, -100.0f, 1000.0f);
        e.oil_press_psi = GetFloat(data, offset, 0.0f, 1000.0f);
        e.fuel_flow_kg_s = GetFloat(data, offset, 0.0f, 100.0f);
        e.manifold_inhg = GetFloat(data, offset, 0.0f, 100.0f);
        e.torque_nm = GetFloat(data, offset, -1.0e6f, 1.0e6f);
    }
    for (int i = 0; i < state.tanks; ++i) {
        state.fuel_kg[i] = GetFloat(data, offset, 0.0f, 1.0e6f);
    }
    return state;
}

} // namespace flytogether
