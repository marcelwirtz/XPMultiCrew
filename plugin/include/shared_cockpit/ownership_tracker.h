#pragma once

#include "shared_cockpit/dataref_sync_protocol.h"
#include "shared_cockpit/shared_cockpit_config.h"

#include <array>
#include <optional>

namespace flytogether {

// Tracks, per DatarefCategory, whether THIS side currently owns it - and
// decides what DatarefSync::Poll() should do with an incoming remote write
// for a dataref in that category. Pure logic, no XPLM/network dependency -
// see tests/ownership_tracker_test.cpp. DatarefSync (which does the actual
// XPLM reads/writes and UDP I/O, and so can't be unit-tested without an
// XPLM stub) composes one of these and asks it for a decision on every
// watched dataref each Poll() tick, mirroring how RemoteAircraft's pure
// dead-reckoning math is kept separate from shared_cockpit_sync.h's XPLM-
// touching glue.
//
// "Claim and tell", not request/grant/deny: touching a dataref in a
// category this side doesn't currently own claims that category
// immediately (Claim()) and broadcasts an OwnershipClaimMessage alongside
// the changed value - no permission step, no waiting on the peer. A peer's
// claim, once received, is accepted unconditionally (OnPeerClaimed()) -
// this side just yields. That's the whole model: whoever last touched a
// category owns it, the same way a real co-pilot can just reach over and
// flip a switch without asking first.
//
// This project briefly used an explicit request/grant/deny handshake
// instead (with per-category timeouts and nonces to keep a late response
// from being misapplied), specifically to close a race this simpler design
// has: if both sides touch the same category at nearly the same moment,
// each one's own claim briefly looks locally valid before the peer's
// crossed claim arrives and flips it back - so for a moment both sides can
// believe they own it, and a value broadcast right in that window can be
// dropped by the side that's about to lose ownership (see
// DatarefSync::ApplyIncomingBytes's ShouldApplyRemoteWrite check). The
// permission-based design closed that race, but in practice its cost -
// having to explicitly release a category and ask for it back for every
// back-and-forth touch - was worse than the rare glitch it prevented, so
// this went back to unconditional claim-and-tell (the user's explicit
// call). The remaining race is self-correcting: the next touch on either
// side re-establishes a single, agreed owner, and it only bites two people
// reaching for the exact same switch at the exact same instant.
class OwnershipTracker {
public:
    // `startsAsOwner`: true for MASTER (owns every category by default,
    // matching Shared Cockpit's existing "master is authoritative unless
    // told otherwise" posture), false for CLIENT (owns nothing until it
    // claims a category, whether by touching a dataref in it or via an
    // explicit companion-app UI action).
    explicit OwnershipTracker(bool startsAsOwner);

    bool Owns(DatarefCategory category) const;

    // Local action ("I'm touching/taking this category now"): if this side
    // already owns `category`, a no-op (nullopt - nothing to send). Else
    // takes ownership immediately and returns the OwnershipClaimMessage to
    // broadcast so the peer yields it.
    std::optional<OwnershipClaimMessage> Claim(DatarefCategory category);

    // A peer's claim arrived - always yields immediately (see class
    // comment: this is what makes the model unilateral, not a request the
    // receiver can refuse).
    void OnPeerClaimed(DatarefCategory category);

    // Decision for an incoming remote write to a dataref in `category`:
    //   true  -> apply it (this side doesn't own the category, so the
    //            remote side is the authority for it)
    //   false -> drop it (this side owns the category - protects its own
    //            authority from a stale/racing update)
    bool ShouldApplyRemoteWrite(DatarefCategory category) const { return !Owns(category); }

private:
    std::array<bool, kDatarefCategoryCount> owns_{};
};

} // namespace flytogether
