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

// Distinct magics for the ownership request/response messages below, so a
// receiver can tell every message shape sharing this UDP channel apart by
// peeking the first 4 bytes before decoding any of them - see
// PeekDatarefSyncChannelMagic().
constexpr uint32_t kOwnershipRequestMagic = 0x46545335;  // "FTS5"
constexpr uint32_t kOwnershipResponseMagic = 0x46545336; // "FTS6"

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

// "I'd like to take over `category`, which the peer currently holds" - see
// ownership_tracker.h's class comment for the full request/grant/deny
// design this is part of. `nonce` is a per-requester, per-category counter
// (OwnershipTracker::RequestCategory bumps it on every *fresh* request,
// but keeps it the same across a resend of a still-pending one) that the
// eventual OwnershipResponseMessage echoes back - this is what lets the
// requester tell a response to its current request apart from a late
// response to an earlier, already-timed-out one for the same category
// (see OwnershipTracker::OnPeerResponded). Not resent with any delivery
// guarantee beyond DatarefSync's existing "send it a few times, UDP is
// best-effort" policy - see DatarefSync::RequestOwnership.
struct OwnershipRequestMessage {
    DatarefCategory category = DatarefCategory::kSystems;
    uint32_t nonce = 0;
};

std::vector<uint8_t> EncodeOwnershipRequestMessage(const OwnershipRequestMessage& msg);
bool DecodeOwnershipRequestMessage(const uint8_t* data, size_t len, OwnershipRequestMessage& out);

// The current owner's Grant/Deny decision for a previously-received
// OwnershipRequestMessage - `nonce` is echoed straight back from that
// request (see above), `grant` is true for a Grant, false for a Deny.
struct OwnershipResponseMessage {
    DatarefCategory category = DatarefCategory::kSystems;
    uint32_t nonce = 0;
    bool grant = false;
};

std::vector<uint8_t> EncodeOwnershipResponseMessage(const OwnershipResponseMessage& msg);
bool DecodeOwnershipResponseMessage(const uint8_t* data, size_t len, OwnershipResponseMessage& out);

} // namespace flytogether
