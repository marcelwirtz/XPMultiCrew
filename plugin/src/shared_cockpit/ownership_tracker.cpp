#include "shared_cockpit/ownership_tracker.h"

namespace flytogether {

OwnershipTracker::OwnershipTracker(bool startsAsOwner) {
    owns_.fill(startsAsOwner);
}

bool OwnershipTracker::Owns(DatarefCategory category) const {
    return owns_[static_cast<size_t>(category)];
}

std::optional<OwnershipRequestMessage> OwnershipTracker::RequestCategory(DatarefCategory category,
                                                                          double now_s) {
    if (Owns(category)) {
        return std::nullopt; // nothing to request - already have it
    }

    PendingRequest& pending = outgoing_[static_cast<size_t>(category)];
    if (pending.active && now_s - pending.sent_at_s < kRequestTimeoutS) {
        // Still waiting on a live request - resend the same one rather
        // than starting a second, differently-nonced request in flight
        // (see class comment).
        return OwnershipRequestMessage{category, pending.nonce};
    }

    pending.active = true;
    pending.nonce = next_nonce_++;
    pending.sent_at_s = now_s;
    return OwnershipRequestMessage{category, pending.nonce};
}

bool OwnershipTracker::IsRequestPending(DatarefCategory category, double now_s) const {
    const PendingRequest& pending = outgoing_[static_cast<size_t>(category)];
    return pending.active && now_s - pending.sent_at_s < kRequestTimeoutS;
}

void OwnershipTracker::OnPeerRequested(DatarefCategory category, uint32_t nonce, double now_s) {
    IncomingRequest& incoming = incoming_[static_cast<size_t>(category)];
    incoming.active = true;
    incoming.nonce = nonce;
    incoming.received_at_s = now_s;
}

bool OwnershipTracker::HasIncomingRequest(DatarefCategory category, double now_s) const {
    const IncomingRequest& incoming = incoming_[static_cast<size_t>(category)];
    return incoming.active && now_s - incoming.received_at_s < kRequestTimeoutS;
}

std::optional<OwnershipResponseMessage> OwnershipTracker::Grant(DatarefCategory category, double now_s) {
    if (!HasIncomingRequest(category, now_s)) {
        return std::nullopt;
    }
    IncomingRequest& incoming = incoming_[static_cast<size_t>(category)];
    const uint32_t nonce = incoming.nonce;
    incoming.active = false;
    owns_[static_cast<size_t>(category)] = false; // give it up - the requester takes over on receipt
    return OwnershipResponseMessage{category, nonce, /*grant=*/true};
}

std::optional<OwnershipResponseMessage> OwnershipTracker::Deny(DatarefCategory category, double now_s) {
    if (!HasIncomingRequest(category, now_s)) {
        return std::nullopt;
    }
    IncomingRequest& incoming = incoming_[static_cast<size_t>(category)];
    const uint32_t nonce = incoming.nonce;
    incoming.active = false;
    return OwnershipResponseMessage{category, nonce, /*grant=*/false};
}

void OwnershipTracker::OnPeerResponded(DatarefCategory category, uint32_t nonce, bool grant, double now_s) {
    PendingRequest& pending = outgoing_[static_cast<size_t>(category)];
    if (!pending.active || pending.nonce != nonce || now_s - pending.sent_at_s >= kRequestTimeoutS) {
        // Not our current request (already timed out, already answered,
        // or a response to a different/earlier one) - discard rather than
        // misapply, see class comment.
        return;
    }
    pending.active = false;
    if (grant) {
        owns_[static_cast<size_t>(category)] = true;
    }
}

} // namespace flytogether
