#pragma once

// Wire format for generic "systems" dataref sync (docs/plan.md section 6).
// Unlike Formation's fixed-schema AircraftStatePacket, this carries an
// arbitrary named dataref's current value, so it works for any switch/
// setting without per-dataref code - as long as both peers are flying the
// identically-named-dataref aircraft (Shared Cockpit's whole premise).
//
// Pure serialization, no XPLM dependency - see tests/dataref_sync_protocol_test.cpp.
// The actual dataref reading/writing lives in shared_cockpit/dataref_sync.h,
// which does need XPLM.

#include <cstdint>
#include <string>
#include <vector>

namespace flytogether {

constexpr uint32_t kDatarefSyncMagic = 0x46545332; // "FTS2"
constexpr uint32_t kDatarefSyncProtocolVersion = 1;
constexpr int kDatarefSyncMaxArrayLen = 8;

enum class DatarefValueType : uint8_t {
    kInt = 0,
    kFloat = 1,
    kDouble = 2,
    kIntArray = 3,
    kFloatArray = 4,
};

struct DatarefValue {
    DatarefValueType type = DatarefValueType::kFloat;
    int int_value = 0;
    float float_value = 0.0f;
    double double_value = 0.0;
    int array_len = 0;
    int int_array[kDatarefSyncMaxArrayLen] = {};
    float float_array[kDatarefSyncMaxArrayLen] = {};

    bool operator==(const DatarefValue& other) const;
    bool operator!=(const DatarefValue& other) const { return !(*this == other); }
};

struct DatarefSyncMessage {
    std::string name;
    DatarefValue value;
};

// Empty result means `name` was too long to fit (shouldn't happen for any
// realistic X-Plane dataref path).
std::vector<uint8_t> EncodeDatarefSyncMessage(const DatarefSyncMessage& msg);

// False means `data` isn't a validly-shaped message (wrong magic/version,
// truncated, or an array_len beyond kDatarefSyncMaxArrayLen).
bool DecodeDatarefSyncMessage(const uint8_t* data, size_t len, DatarefSyncMessage& out);

} // namespace flytogether
