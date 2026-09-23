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

#include "shared_cockpit/shared_cockpit_config.h"

#include <cstdint>
#include <string>
#include <vector>

namespace flytogether {

constexpr uint32_t kDatarefSyncMagic = 0x46545332; // "FTS2"
constexpr uint32_t kDatarefSyncProtocolVersion = 1;
constexpr int kDatarefSyncMaxArrayLen = 8;

// Distinct magic for the ownership claim message below, so a receiver can
// tell every message shape sharing this UDP channel apart by peeking the
// first 4 bytes before decoding any of them - see
// PeekDatarefSyncChannelMagic(). Reuses the byte value an earlier
// request/grant/deny design used for its request message (see
// ownership_tracker.h's class comment for why that design was replaced) -
// an old build's OwnershipRequestMessage and this build's
// OwnershipClaimMessage happen to share a magic but decode different
// payloads; DecodeOwnershipClaimMessage's own length/category checks
// reject anything that doesn't parse as this shape.
constexpr uint32_t kOwnershipClaimMagic = 0x46545335; // "FTS5"

// Reads just the magic from the front of a datagram received on
// DatarefSync's UDP channel, without committing to decoding it as either
// message shape - `data` must have at least 4 bytes (shorter buffers
// can't be either message and should just be dropped by the caller).
uint32_t PeekDatarefSyncChannelMagic(const uint8_t* data, size_t len);

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

// "I'm taking over `category` right now" - a one-way notify, not a
// request: the sender has already taken local ownership by the time this
// goes out (see OwnershipTracker::Claim), and the receiver is expected to
// yield unconditionally (OwnershipTracker::OnPeerClaimed) rather than
// answer it. See ownership_tracker.h's class comment for the claim-and-
// tell design this is part of. Not resent with any delivery guarantee
// beyond DatarefSync's existing "send it a few times, UDP is best-effort"
// policy - see DatarefSync::ClaimOwnership.
struct OwnershipClaimMessage {
    DatarefCategory category = DatarefCategory::kSystems;
};

std::vector<uint8_t> EncodeOwnershipClaimMessage(const OwnershipClaimMessage& msg);
bool DecodeOwnershipClaimMessage(const uint8_t* data, size_t len, OwnershipClaimMessage& out);

} // namespace flytogether
