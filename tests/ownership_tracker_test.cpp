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
    assert(master.ShouldBroadcastLocalChange(DatarefCategory::kEngine));
    assert(!master.ShouldApplyRemoteWrite(DatarefCategory::kEngine));
    std::printf("TestMasterStartsOwningEveryCategory: OK\n");
}

void TestClientStartsOwningNothing() {
    OwnershipTracker client(/*startsAsOwner=*/false);
    assert(!client.Owns(DatarefCategory::kSystems));
    assert(!client.Owns(DatarefCategory::kEngine));
    assert(!client.Owns(DatarefCategory::kAvionics));
    assert(!client.ShouldBroadcastLocalChange(DatarefCategory::kEngine));
    assert(client.ShouldApplyRemoteWrite(DatarefCategory::kEngine));
    std::printf("TestClientStartsOwningNothing: OK\n");
}

void TestRequestingAnAlreadyOwnedCategoryIsANoop() {
    OwnershipTracker master(/*startsAsOwner=*/true);
    const auto msg = master.RequestCategory(DatarefCategory::kEngine, /*now_s=*/0.0);
    assert(!msg.has_value());
    assert(!master.IsRequestPending(DatarefCategory::kEngine, 0.0));
    std::printf("TestRequestingAnAlreadyOwnedCategoryIsANoop: OK\n");
}

void TestRequestCategoryStartsAPendingRequest() {
    OwnershipTracker client(/*startsAsOwner=*/false);
    const auto msg = client.RequestCategory(DatarefCategory::kEngine, /*now_s=*/10.0);
    assert(msg.has_value());
    assert(msg->category == DatarefCategory::kEngine);
    assert(!client.Owns(DatarefCategory::kEngine)); // not granted yet, just asked
    assert(client.IsRequestPending(DatarefCategory::kEngine, 10.0));
    // A different category is untouched.
    assert(!client.IsRequestPending(DatarefCategory::kAvionics, 10.0));
    std::printf("TestRequestCategoryStartsAPendingRequest: OK\n");
}

void TestRepeatRequestWhileStillPendingResendsSameNonce() {
    OwnershipTracker client(/*startsAsOwner=*/false);
    const auto first = client.RequestCategory(DatarefCategory::kEngine, /*now_s=*/0.0);
    const auto second = client.RequestCategory(DatarefCategory::kEngine, /*now_s=*/5.0);
    assert(first.has_value() && second.has_value());
    assert(first->nonce == second->nonce);
    // Still pending relative to the ORIGINAL sent_at (0.0), not reset by
    // the repeat click at 5.0 - just short of the 25s timeout measured
    // from 0.0.
    assert(client.IsRequestPending(DatarefCategory::kEngine, 24.0));
    assert(!client.IsRequestPending(DatarefCategory::kEngine, 26.0));
    std::printf("TestRepeatRequestWhileStillPendingResendsSameNonce: OK\n");
}

void TestRequestTimesOutAndCanBeRetriedWithAFreshNonce() {
    OwnershipTracker client(/*startsAsOwner=*/false);
    const auto first = client.RequestCategory(DatarefCategory::kEngine, /*now_s=*/0.0);
    assert(!client.IsRequestPending(DatarefCategory::kEngine, 30.0)); // past the 25s timeout
    const auto retry = client.RequestCategory(DatarefCategory::kEngine, /*now_s=*/30.0);
    assert(retry.has_value());
    assert(retry->nonce != first->nonce); // a genuinely fresh request, not a resend
    assert(client.IsRequestPending(DatarefCategory::kEngine, 30.0));
    std::printf("TestRequestTimesOutAndCanBeRetriedWithAFreshNonce: OK\n");
}

void TestIncomingRequestGrantTransfersOwnershipAndClearsPrompt() {
    OwnershipTracker master(/*startsAsOwner=*/true);
    assert(!master.HasIncomingRequest(DatarefCategory::kEngine, 0.0));
    master.OnPeerRequested(DatarefCategory::kEngine, /*nonce=*/7, /*now_s=*/0.0);
    assert(master.HasIncomingRequest(DatarefCategory::kEngine, 1.0));

    const auto response = master.Grant(DatarefCategory::kEngine, /*now_s=*/1.0);
    assert(response.has_value());
    assert(response->category == DatarefCategory::kEngine);
    assert(response->nonce == 7);
    assert(response->grant == true);
    assert(!master.Owns(DatarefCategory::kEngine)); // gave it up immediately
    assert(!master.HasIncomingRequest(DatarefCategory::kEngine, 1.0)); // prompt cleared

    // Nothing left to grant a second time.
    assert(!master.Grant(DatarefCategory::kEngine, 1.0).has_value());
    std::printf("TestIncomingRequestGrantTransfersOwnershipAndClearsPrompt: OK\n");
}

void TestIncomingRequestDenyKeepsOwnershipAndClearsPrompt() {
    OwnershipTracker master(/*startsAsOwner=*/true);
    master.OnPeerRequested(DatarefCategory::kEngine, /*nonce=*/3, /*now_s=*/0.0);

    const auto response = master.Deny(DatarefCategory::kEngine, /*now_s=*/1.0);
    assert(response.has_value());
    assert(response->nonce == 3);
    assert(response->grant == false);
    assert(master.Owns(DatarefCategory::kEngine)); // kept it
    assert(!master.HasIncomingRequest(DatarefCategory::kEngine, 1.0));
    std::printf("TestIncomingRequestDenyKeepsOwnershipAndClearsPrompt: OK\n");
}

void TestIncomingRequestExpiresWithoutAnAnswer() {
    OwnershipTracker master(/*startsAsOwner=*/true);
    master.OnPeerRequested(DatarefCategory::kEngine, /*nonce=*/1, /*now_s=*/0.0);
    assert(master.HasIncomingRequest(DatarefCategory::kEngine, 24.0));
    assert(!master.HasIncomingRequest(DatarefCategory::kEngine, 26.0)); // stale - requester's own timeout fired

    // A stale incoming request can't be granted or denied any more.
    assert(!master.Grant(DatarefCategory::kEngine, 26.0).has_value());
    assert(!master.Deny(DatarefCategory::kEngine, 26.0).has_value());
    assert(master.Owns(DatarefCategory::kEngine)); // untouched
    std::printf("TestIncomingRequestExpiresWithoutAnAnswer: OK\n");
}

void TestGrantOrDenyWithNoLiveIncomingRequestIsANoop() {
    OwnershipTracker master(/*startsAsOwner=*/true);
    assert(!master.Grant(DatarefCategory::kEngine, 0.0).has_value());
    assert(!master.Deny(DatarefCategory::kEngine, 0.0).has_value());
    std::printf("TestGrantOrDenyWithNoLiveIncomingRequestIsANoop: OK\n");
}

void TestEndToEndRequestGrantRoundTrip() {
    OwnershipTracker master(/*startsAsOwner=*/true);
    OwnershipTracker client(/*startsAsOwner=*/false);

    const auto request = client.RequestCategory(DatarefCategory::kEngine, /*now_s=*/0.0);
    assert(request.has_value());

    master.OnPeerRequested(request->category, request->nonce, /*now_s=*/0.1);
    const auto response = master.Grant(DatarefCategory::kEngine, /*now_s=*/0.2);
    assert(response.has_value());
    assert(!master.Owns(DatarefCategory::kEngine));

    client.OnPeerResponded(response->category, response->nonce, response->grant, /*now_s=*/0.3);
    assert(client.Owns(DatarefCategory::kEngine));
    assert(!client.IsRequestPending(DatarefCategory::kEngine, 0.3));
    std::printf("TestEndToEndRequestGrantRoundTrip: OK\n");
}

void TestEndToEndRequestDenyRoundTrip() {
    OwnershipTracker master(/*startsAsOwner=*/true);
    OwnershipTracker client(/*startsAsOwner=*/false);

    const auto request = client.RequestCategory(DatarefCategory::kEngine, /*now_s=*/0.0);
    master.OnPeerRequested(request->category, request->nonce, /*now_s=*/0.1);
    const auto response = master.Deny(DatarefCategory::kEngine, /*now_s=*/0.2);

    client.OnPeerResponded(response->category, response->nonce, response->grant, /*now_s=*/0.3);
    assert(!client.Owns(DatarefCategory::kEngine));
    assert(!client.IsRequestPending(DatarefCategory::kEngine, 0.3)); // free to request again
    std::printf("TestEndToEndRequestDenyRoundTrip: OK\n");
}

void TestLateResponseAfterOwnTimeoutIsDiscarded() {
    // The scenario OwnershipTracker's class comment calls out by name: a
    // first request times out, a second (freshly-nonced) one is already
    // in flight, and only THEN does a response to the first one show up.
    OwnershipTracker client(/*startsAsOwner=*/false);
    const auto first = client.RequestCategory(DatarefCategory::kEngine, /*now_s=*/0.0);
    assert(!client.IsRequestPending(DatarefCategory::kEngine, 30.0)); // first request timed out
    const auto second = client.RequestCategory(DatarefCategory::kEngine, /*now_s=*/30.0);
    assert(first->nonce != second->nonce);

    // A late Grant for the FIRST (abandoned) request arrives - must not be
    // mistaken for a response to the second, currently-live one.
    client.OnPeerResponded(DatarefCategory::kEngine, first->nonce, /*grant=*/true, /*now_s=*/32.0);
    assert(!client.Owns(DatarefCategory::kEngine));
    assert(client.IsRequestPending(DatarefCategory::kEngine, 32.0)); // second request still live

    // The real response to the second request still works normally.
    client.OnPeerResponded(DatarefCategory::kEngine, second->nonce, /*grant=*/true, /*now_s=*/33.0);
    assert(client.Owns(DatarefCategory::kEngine));
    std::printf("TestLateResponseAfterOwnTimeoutIsDiscarded: OK\n");
}

void TestResponseAfterLocalRequestAlreadyTimedOutIsDiscarded() {
    OwnershipTracker client(/*startsAsOwner=*/false);
    client.RequestCategory(DatarefCategory::kEngine, /*now_s=*/0.0);
    // No retry this time - just a very late response arriving after the
    // 25s window, with nothing newer pending to confuse it with.
    client.OnPeerResponded(DatarefCategory::kEngine, /*nonce=*/1, /*grant=*/true, /*now_s=*/26.0);
    assert(!client.Owns(DatarefCategory::kEngine));
    std::printf("TestResponseAfterLocalRequestAlreadyTimedOutIsDiscarded: OK\n");
}

void TestRequestingOneCategoryDoesNotAffectAnother() {
    OwnershipTracker master(/*startsAsOwner=*/true);
    master.OnPeerRequested(DatarefCategory::kEngine, /*nonce=*/1, /*now_s=*/0.0);
    master.Grant(DatarefCategory::kEngine, /*now_s=*/1.0);
    assert(!master.Owns(DatarefCategory::kEngine));
    assert(master.Owns(DatarefCategory::kAvionics));
    assert(master.Owns(DatarefCategory::kSystems));
    std::printf("TestRequestingOneCategoryDoesNotAffectAnother: OK\n");
}

} // namespace

int main() {
    TestMasterStartsOwningEveryCategory();
    TestClientStartsOwningNothing();
    TestRequestingAnAlreadyOwnedCategoryIsANoop();
    TestRequestCategoryStartsAPendingRequest();
    TestRepeatRequestWhileStillPendingResendsSameNonce();
    TestRequestTimesOutAndCanBeRetriedWithAFreshNonce();
    TestIncomingRequestGrantTransfersOwnershipAndClearsPrompt();
    TestIncomingRequestDenyKeepsOwnershipAndClearsPrompt();
    TestIncomingRequestExpiresWithoutAnAnswer();
    TestGrantOrDenyWithNoLiveIncomingRequestIsANoop();
    TestEndToEndRequestGrantRoundTrip();
    TestEndToEndRequestDenyRoundTrip();
    TestLateResponseAfterOwnTimeoutIsDiscarded();
    TestResponseAfterLocalRequestAlreadyTimedOutIsDiscarded();
    TestRequestingOneCategoryDoesNotAffectAnother();
    std::printf("All ownership_tracker tests passed.\n");
    return 0;
}
