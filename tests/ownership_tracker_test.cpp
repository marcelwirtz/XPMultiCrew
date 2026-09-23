// Pure-logic regression test for DatarefSync's ownership arbitration,
// extracted into OwnershipTracker so it's testable without an XPLM stub -
// same reasoning as tests/remote_aircraft_test.cpp. No XPLM SDK or
// networking involved.

#include "shared_cockpit/ownership_tracker.h"

#include <cassert>
#include <cstdio>

using namespace flytogether;

namespace {

void TestMasterStartsOwningEveryCategory() {
    OwnershipTracker master(/*startsAsOwner=*/true);
    assert(master.Owns(DatarefCategory::kSystems));
    assert(master.Owns(DatarefCategory::kEngine));
    assert(master.Owns(DatarefCategory::kAvionics));
    assert(!master.ShouldApplyRemoteWrite(DatarefCategory::kEngine));
    std::printf("TestMasterStartsOwningEveryCategory: OK\n");
}

void TestClientStartsOwningNothing() {
    OwnershipTracker client(/*startsAsOwner=*/false);
    assert(!client.Owns(DatarefCategory::kSystems));
    assert(!client.Owns(DatarefCategory::kEngine));
    assert(!client.Owns(DatarefCategory::kAvionics));
    assert(client.ShouldApplyRemoteWrite(DatarefCategory::kEngine));
    std::printf("TestClientStartsOwningNothing: OK\n");
}

void TestClaimingAnAlreadyOwnedCategoryIsANoop() {
    OwnershipTracker master(/*startsAsOwner=*/true);
    const auto msg = master.Claim(DatarefCategory::kEngine);
    assert(!msg.has_value());
    assert(master.Owns(DatarefCategory::kEngine)); // untouched
    std::printf("TestClaimingAnAlreadyOwnedCategoryIsANoop: OK\n");
}

void TestClaimingAnUnownedCategoryTakesItImmediately() {
    OwnershipTracker client(/*startsAsOwner=*/false);
    const auto msg = client.Claim(DatarefCategory::kEngine);
    assert(msg.has_value());
    assert(msg->category == DatarefCategory::kEngine);
    assert(client.Owns(DatarefCategory::kEngine)); // instant, no round trip
    // A different category is untouched.
    assert(!client.Owns(DatarefCategory::kAvionics));
    std::printf("TestClaimingAnUnownedCategoryTakesItImmediately: OK\n");
}

void TestPeerClaimAlwaysYieldsOwnership() {
    OwnershipTracker master(/*startsAsOwner=*/true);
    assert(master.Owns(DatarefCategory::kEngine));
    master.OnPeerClaimed(DatarefCategory::kEngine);
    assert(!master.Owns(DatarefCategory::kEngine)); // gave it up unconditionally
    assert(master.ShouldApplyRemoteWrite(DatarefCategory::kEngine));
    // Other categories untouched.
    assert(master.Owns(DatarefCategory::kAvionics));
    std::printf("TestPeerClaimAlwaysYieldsOwnership: OK\n");
}

void TestPeerClaimOnAlreadyUnownedCategoryIsHarmless() {
    OwnershipTracker client(/*startsAsOwner=*/false);
    assert(!client.Owns(DatarefCategory::kEngine));
    client.OnPeerClaimed(DatarefCategory::kEngine); // peer re-asserts what it already had
    assert(!client.Owns(DatarefCategory::kEngine));
    std::printf("TestPeerClaimOnAlreadyUnownedCategoryIsHarmless: OK\n");
}

void TestEndToEndClaimHandsOverOwnership() {
    OwnershipTracker master(/*startsAsOwner=*/true);
    OwnershipTracker client(/*startsAsOwner=*/false);

    const auto claim = client.Claim(DatarefCategory::kEngine);
    assert(claim.has_value());
    assert(client.Owns(DatarefCategory::kEngine)); // client took it locally right away

    master.OnPeerClaimed(claim->category);
    assert(!master.Owns(DatarefCategory::kEngine)); // master yields on receipt
    std::printf("TestEndToEndClaimHandsOverOwnership: OK\n");
}

void TestClaimingOneCategoryDoesNotAffectAnother() {
    OwnershipTracker client(/*startsAsOwner=*/false);
    client.Claim(DatarefCategory::kEngine);
    assert(client.Owns(DatarefCategory::kEngine));
    assert(!client.Owns(DatarefCategory::kAvionics));
    assert(!client.Owns(DatarefCategory::kSystems));
    std::printf("TestClaimingOneCategoryDoesNotAffectAnother: OK\n");
}

} // namespace

int main() {
    TestMasterStartsOwningEveryCategory();
    TestClientStartsOwningNothing();
    TestClaimingAnAlreadyOwnedCategoryIsANoop();
    TestClaimingAnUnownedCategoryTakesItImmediately();
    TestPeerClaimAlwaysYieldsOwnership();
    TestPeerClaimOnAlreadyUnownedCategoryIsHarmless();
    TestEndToEndClaimHandsOverOwnership();
    TestClaimingOneCategoryDoesNotAffectAnother();
    std::printf("All ownership_tracker tests passed.\n");
    return 0;
}
