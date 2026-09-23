#include "shared_cockpit/ownership_tracker.h"

namespace flytogether {

OwnershipTracker::OwnershipTracker(bool startsAsOwner) {
    owns_.fill(startsAsOwner);
}

bool OwnershipTracker::Owns(DatarefCategory category) const {
    return owns_[static_cast<size_t>(category)];
}

std::optional<OwnershipClaimMessage> OwnershipTracker::Claim(DatarefCategory category) {
    if (Owns(category)) {
        return std::nullopt; // already have it - nothing to claim/send
    }
    owns_[static_cast<size_t>(category)] = true;
    return OwnershipClaimMessage{category};
}

void OwnershipTracker::OnPeerClaimed(DatarefCategory category) {
    owns_[static_cast<size_t>(category)] = false;
}

} // namespace flytogether
