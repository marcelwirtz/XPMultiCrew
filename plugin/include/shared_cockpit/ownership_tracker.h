#pragma once

#include "shared_cockpit/dataref_sync_protocol.h"
#include "shared_cockpit/shared_cockpit_config.h"

#include <array>
#include <optional>

namespace flytogether {

// Tracks, per DatarefCategory, whether THIS side currently owns it - and
// decides what DatarefSync::Poll() should do with a locally-detected
// change or an incoming remote write for a dataref in that category.
// Pure logic, no XPLM/network dependency - see
// tests/ownership_tracker_test.cpp. DatarefSync (which does the actual
// XPLM reads/writes and UDP I/O, and so can't be unit-tested without an
// XPLM stub) composes one of these and asks it for a decision on every
// watched dataref each Poll() tick, mirroring how RemoteAircraft's pure
// dead-reckoning math is kept separate from shared_cockpit_sync.h's XPLM-
// touching glue.
//
// Exactly two participants in a Shared Cockpit session. Taking a category
// the peer currently holds is a real request/grant/deny handshake, not a
// unilateral claim: the non-owner asks (RequestCategory), the owner
// decides (Grant/Deny), and the requester applies the outcome
// (OnPeerResponded). This replaced an earlier "claim and tell, no
// accept/reject" design (see git history) that traded correctness for
// simplicity - it could never deadlock or need a timeout, but left a real
// window where both sides could believe they owned the same category if
// their claims crossed in flight.
//
// If the owner doesn't respond within kRequestTimeoutS, the request is
// treated as denied - but this is deliberately implemented as the
// REQUESTER silently giving up (IsRequestPending() reads false once
// expired, letting RequestCategory() start a fresh request), not as the
// owner proactively sending a Deny on a timer. No separate wire message or
// per-owner clock is needed for that: from the requester's point of view,
// "no response for kRequestTimeoutS" and "an explicit Deny" already look
// identical (still not the owner), so there's nothing a timed auto-Deny
// message would add. The owner side's own HasIncomingRequest() uses the
// same expiry so a stale prompt ("co-pilot wants Engine") doesn't linger
// in the UI once the requester has almost certainly already given up.
//
// A nonce (bumped on every FRESH RequestCategory() call, kept unchanged
// across a resend of a still-pending one) is what makes the timeout safe
// against a late response: without it, a Grant/Deny that finally arrives
// for an old, already-abandoned request could be misapplied to whatever
// NEW request happens to be pending for that category by the time it
// shows up. OnPeerResponded() only applies a response whose nonce matches
// the currently-pending request's nonce, discarding anything else.
class OwnershipTracker {
public:
    // How long a request waits for a Grant/Deny before the requester gives
    // up (see class comment). Long enough to survive a moment's
    // inattention (radio call, checklist item) without feeling broken,
    // short enough that "no answer" doesn't strand the requester for the
    // rest of the flight.
    static constexpr double kRequestTimeoutS = 25.0;

    // `startsAsOwner`: true for MASTER (owns every category by default,
    // matching Shared Cockpit's existing "master is authoritative unless
    // told otherwise" posture), false for CLIENT (owns nothing until it
    // successfully requests a category).
    explicit OwnershipTracker(bool startsAsOwner);

    bool Owns(DatarefCategory category) const;

    // Local UI action ("I'd like category X"):
    //  - already owns it: no-op, returns nullopt (nothing to send).
    //  - doesn't own it, no live pending request for it: starts a fresh
    //    request (bumps the nonce, records `now_s` as when it was sent)
    //    and returns the OwnershipRequestMessage to broadcast.
    //  - doesn't own it, a request for it is already live (not yet timed
    //    out): returns that SAME request again (same nonce, sent_at not
    //    reset) so the caller can just resend it - a repeat click doesn't
    //    restart the timeout clock or spawn a second, differently-nonced
    //    request in flight.
    std::optional<OwnershipRequestMessage> RequestCategory(DatarefCategory category, double now_s);

    // True if this side is currently waiting on a response to its own
    // request for `category`, and that request hasn't yet timed out (see
    // class comment) - drives the UI's "pending…" state.
    bool IsRequestPending(DatarefCategory category, double now_s) const;

    // A peer's request arrived - remember it (overwriting any earlier
    // pending request for the same category, e.g. a resend or a fresh
    // request after the peer's own previous one expired) so the UI can
    // offer to Grant/Deny it.
    void OnPeerRequested(DatarefCategory category, uint32_t nonce, double now_s);

    // True if there's a peer request for `category` awaiting a local
    // Grant/Deny decision, and it hasn't gone stale (kRequestTimeoutS) -
    // drives the UI's Grant/Deny prompt. A stale one reads as false (as if
    // never asked): by then the requester has almost certainly already
    // given up on its own timeout, so prompting for a decision that no
    // longer matters to anyone would just be confusing.
    bool HasIncomingRequest(DatarefCategory category, double now_s) const;

    // Local UI action: grant a pending incoming request for `category` -
    // gives up local ownership immediately and returns the response to
    // broadcast. No-op (nullopt) if there's no live incoming request for
    // it (e.g. it already went stale, or was already answered).
    std::optional<OwnershipResponseMessage> Grant(DatarefCategory category, double now_s);

    // Local UI action: deny a pending incoming request for `category` -
    // keeps local ownership, returns the response to broadcast. No-op
    // (nullopt) under the same conditions as Grant().
    std::optional<OwnershipResponseMessage> Deny(DatarefCategory category, double now_s);

    // A response to one of OUR OWN earlier requests arrived. Applied only
    // if `nonce` matches the currently-pending (and not yet timed-out)
    // request for `category` - see class comment for why a mismatched or
    // absent pending request means this is a late response to an
    // abandoned request and must be discarded rather than misapplied.
    // `grant=true` takes ownership; `grant=false` (or a discarded
    // mismatch) just clears the pending state, same end result as the
    // requester's own timeout.
    void OnPeerResponded(DatarefCategory category, uint32_t nonce, bool grant, double now_s);

    // Decision for a dataref that changed *locally* (detected by
    // DatarefSync::Poll() comparing against its cached value) whose
    // category is `category`:
    //   true  -> broadcast it (this side owns the category)
    //   false -> this side doesn't own it; the caller should revert the
    //            dataref to its last-known-good value instead of
    //            broadcasting, so an unauthorized local touch (e.g. a
    //            hardware switch wired straight into X-Plane) gets one
    //            deterministic snap-back instead of fighting the network
    //            silently.
    bool ShouldBroadcastLocalChange(DatarefCategory category) const { return Owns(category); }

    // Decision for an incoming remote write to a dataref in `category`:
    //   true  -> apply it (this side doesn't own the category, so the
    //            remote side is the authority for it)
    //   false -> drop it (this side owns the category - protects its own
    //            authority from a stale/racing update)
    bool ShouldApplyRemoteWrite(DatarefCategory category) const { return !Owns(category); }

private:
    struct PendingRequest {
        bool active = false;
        uint32_t nonce = 0;
        double sent_at_s = 0.0;
    };
    struct IncomingRequest {
        bool active = false;
        uint32_t nonce = 0;
        double received_at_s = 0.0;
    };

    std::array<bool, kDatarefCategoryCount> owns_{};
    std::array<PendingRequest, kDatarefCategoryCount> outgoing_{};
    std::array<IncomingRequest, kDatarefCategoryCount> incoming_{};
    uint32_t next_nonce_ = 1; // 0 is never issued, so a default-constructed message is never mistaken for a real one
};

} // namespace flytogether
