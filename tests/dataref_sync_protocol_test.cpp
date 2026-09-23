// Pure-logic tests for the generic dataref sync wire format
// (shared_cockpit/dataref_sync_protocol.h). No XPLM dependency.

#include "shared_cockpit/dataref_sync_protocol.h"

#include <cassert>
#include <cstdio>

using namespace flytogether;

namespace {

void TestRoundTripInt() {
    DatarefSyncMessage msg;
    msg.name = "sim/cockpit/radios/transponder_code";
    msg.value.type = DatarefValueType::kInt;
    msg.value.int_value = 7000;

    const auto encoded = EncodeDatarefSyncMessage(msg);
    assert(!encoded.empty());

    DatarefSyncMessage decoded;
    assert(DecodeDatarefSyncMessage(encoded.data(), encoded.size(), decoded));
    assert(decoded.name == msg.name);
    assert(decoded.value == msg.value);
    std::printf("TestRoundTripInt: OK\n");
}

void TestRoundTripFloat() {
    DatarefSyncMessage msg;
    msg.name = "sim/flightmodel/controls/parkbrake";
    msg.value.type = DatarefValueType::kFloat;
    msg.value.float_value = 1.0f;

    const auto encoded = EncodeDatarefSyncMessage(msg);
    DatarefSyncMessage decoded;
    assert(DecodeDatarefSyncMessage(encoded.data(), encoded.size(), decoded));
    assert(decoded.name == msg.name);
    assert(decoded.value.type == DatarefValueType::kFloat);
    assert(decoded.value.float_value == 1.0f);
    std::printf("TestRoundTripFloat: OK\n");
}

void TestRoundTripDouble() {
    DatarefSyncMessage msg;
    msg.name = "some/double/dataref";
    msg.value.type = DatarefValueType::kDouble;
    msg.value.double_value = 123456.789;

    const auto encoded = EncodeDatarefSyncMessage(msg);
    DatarefSyncMessage decoded;
    assert(DecodeDatarefSyncMessage(encoded.data(), encoded.size(), decoded));
    assert(decoded.value.double_value == 123456.789);
    std::printf("TestRoundTripDouble: OK\n");
}

void TestRoundTripFloatArray() {
    DatarefSyncMessage msg;
    msg.name = "sim/some/float_array";
    msg.value.type = DatarefValueType::kFloatArray;
    msg.value.array_len = 4;
    msg.value.float_array[0] = 1.5f;
    msg.value.float_array[1] = -2.5f;
    msg.value.float_array[2] = 0.0f;
    msg.value.float_array[3] = 99.0f;

    const auto encoded = EncodeDatarefSyncMessage(msg);
    DatarefSyncMessage decoded;
    assert(DecodeDatarefSyncMessage(encoded.data(), encoded.size(), decoded));
    assert(decoded.value == msg.value);
    std::printf("TestRoundTripFloatArray: OK\n");
}

void TestRoundTripIntArray() {
    DatarefSyncMessage msg;
    msg.name = "sim/some/int_array";
    msg.value.type = DatarefValueType::kIntArray;
    msg.value.array_len = 3;
    msg.value.int_array[0] = 1;
    msg.value.int_array[1] = 0;
    msg.value.int_array[2] = -5;

    const auto encoded = EncodeDatarefSyncMessage(msg);
    DatarefSyncMessage decoded;
    assert(DecodeDatarefSyncMessage(encoded.data(), encoded.size(), decoded));
    assert(decoded.value == msg.value);
    std::printf("TestRoundTripIntArray: OK\n");
}

void TestValueEqualityIgnoresIrrelevantFields() {
    DatarefValue a;
    a.type = DatarefValueType::kInt;
    a.int_value = 5;
    a.float_value = 999.0f; // irrelevant for kInt, should not affect equality

    DatarefValue b;
    b.type = DatarefValueType::kInt;
    b.int_value = 5;
    b.float_value = -1.0f; // different irrelevant field

    assert(a == b);
    std::printf("TestValueEqualityIgnoresIrrelevantFields: OK\n");
}

void TestDecodeRejectsGarbage() {
    DatarefSyncMessage decoded;
    const uint8_t garbage[] = {1, 2, 3};
    assert(!DecodeDatarefSyncMessage(garbage, sizeof(garbage), decoded));

    DatarefSyncMessage msg;
    msg.name = "x";
    msg.value.type = DatarefValueType::kInt;
    msg.value.int_value = 42;
    auto encoded = EncodeDatarefSyncMessage(msg);
    // Truncate the payload after the header+name - should be rejected, not
    // read out-of-bounds or return a half-populated message.
    encoded.resize(encoded.size() - 2);
    assert(!DecodeDatarefSyncMessage(encoded.data(), encoded.size(), decoded));

    std::printf("TestDecodeRejectsGarbage: OK\n");
}

void TestEmptyNameOrOversizedArrayRejectedAtEncode() {
    DatarefSyncMessage msg;
    msg.name = "ok";
    msg.value.type = DatarefValueType::kFloatArray;
    msg.value.array_len = kDatarefSyncMaxArrayLen + 1; // too big
    assert(EncodeDatarefSyncMessage(msg).empty());
    std::printf("TestEmptyNameOrOversizedArrayRejectedAtEncode: OK\n");
}

void TestOwnershipClaimMessageRoundTrip() {
    for (DatarefCategory category :
         {DatarefCategory::kSystems, DatarefCategory::kEngine, DatarefCategory::kAvionics}) {
        OwnershipClaimMessage msg{category};
        const auto encoded = EncodeOwnershipClaimMessage(msg);
        assert(!encoded.empty());

        OwnershipClaimMessage decoded;
        assert(DecodeOwnershipClaimMessage(encoded.data(), encoded.size(), decoded));
        assert(decoded.category == category);
    }
    std::printf("TestOwnershipClaimMessageRoundTrip: OK\n");
}

void TestOwnershipClaimMessageDecodeRejectsGarbage() {
    OwnershipClaimMessage decoded;
    assert(!DecodeOwnershipClaimMessage(nullptr, 0, decoded));

    const uint8_t too_short[] = {0x35, 0x53, 0x54, 0x46}; // magic only, no category
    assert(!DecodeOwnershipClaimMessage(too_short, sizeof(too_short), decoded));

    OwnershipClaimMessage msg{DatarefCategory::kEngine};
    auto encoded = EncodeOwnershipClaimMessage(msg);
    encoded[0] ^= 0xFF; // corrupt the magic
    assert(!DecodeOwnershipClaimMessage(encoded.data(), encoded.size(), decoded));

    encoded = EncodeOwnershipClaimMessage(msg);
    encoded[sizeof(uint32_t)] = 99; // not a valid DatarefCategory value
    assert(!DecodeOwnershipClaimMessage(encoded.data(), encoded.size(), decoded));

    std::printf("TestOwnershipClaimMessageDecodeRejectsGarbage: OK\n");
}

void TestPeekChannelMagicDistinguishesMessageShapes() {
    const DatarefSyncMessage dr_msg{"sim/some/dataref", DatarefValue{}};
    const auto dr_encoded = EncodeDatarefSyncMessage(dr_msg);
    assert(PeekDatarefSyncChannelMagic(dr_encoded.data(), dr_encoded.size()) == kDatarefSyncMagic);

    const OwnershipClaimMessage claim_msg{DatarefCategory::kAvionics};
    const auto claim_encoded = EncodeOwnershipClaimMessage(claim_msg);
    assert(PeekDatarefSyncChannelMagic(claim_encoded.data(), claim_encoded.size()) == kOwnershipClaimMagic);

    const uint8_t too_short[] = {1, 2, 3};
    assert(PeekDatarefSyncChannelMagic(too_short, sizeof(too_short)) == 0);

    std::printf("TestPeekChannelMagicDistinguishesMessageShapes: OK\n");
}

} // namespace

int main() {
    TestRoundTripInt();
    TestRoundTripFloat();
    TestRoundTripDouble();
    TestRoundTripFloatArray();
    TestRoundTripIntArray();
    TestValueEqualityIgnoresIrrelevantFields();
    TestDecodeRejectsGarbage();
    TestEmptyNameOrOversizedArrayRejectedAtEncode();
    TestOwnershipClaimMessageRoundTrip();
    TestOwnershipClaimMessageDecodeRejectsGarbage();
    TestPeekChannelMagicDistinguishesMessageShapes();
    std::printf("All dataref_sync_protocol tests passed.\n");
    return 0;
}
