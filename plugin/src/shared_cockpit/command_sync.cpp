#include "shared_cockpit/command_sync.h"

namespace flytogether {

void CommandSync::Start(const std::vector<CommandSyncSpec>& specs, uint32_t sender_id, SendFn send,
                        LocalPressFn on_local_press) {
    Stop();
    sender_id_ = sender_id;
    send_ = std::move(send);
    on_local_press_ = std::move(on_local_press);
    for (const auto& spec : specs) {
        if (by_name_.count(spec.name)) {
            continue;
        }
        XPLMCommandRef ref = XPLMFindCommand(spec.name.c_str());
        if (!ref) {
            continue; // not in this X-Plane/aircraft - silently skipped, like unknown datarefs
        }
        auto entry = std::make_unique<Entry>();
        entry->owner = this;
        entry->ref = ref;
        entry->spec = spec;
        // Before X-Plane's own handler, and always passing the command on,
        // so the local press works exactly as without us.
        XPLMRegisterCommandHandler(ref, &CommandSync::Handler, 1, entry.get());
        by_name_[spec.name] = entry.get();
        entries_.push_back(std::move(entry));
    }
}

void CommandSync::Stop() {
    for (const auto& [name, since] : remote_held_) {
        const auto it = by_name_.find(name);
        if (it != by_name_.end()) {
            replaying_ = true;
            XPLMCommandEnd(it->second->ref);
            replaying_ = false;
        }
    }
    remote_held_.clear();
    for (const auto& entry : entries_) {
        XPLMUnregisterCommandHandler(entry->ref, &CommandSync::Handler, 1, entry.get());
    }
    entries_.clear();
    by_name_.clear();
}

int CommandSync::Handler(XPLMCommandRef /*cmd*/, XPLMCommandPhase phase, void* refcon) {
    auto* entry = static_cast<Entry*>(refcon);
    CommandSync* self = entry->owner;
    if (self->replaying_) {
        return 1; // the peer's press we're replaying - don't echo it back
    }
    if (phase == xplm_CommandBegin) {
        if (self->on_local_press_) self->on_local_press_(entry->spec.category);
        self->SendPhase(*entry, CommandPhase::kBegin);
    } else if (phase == xplm_CommandEnd) {
        self->SendPhase(*entry, CommandPhase::kEnd);
    }
    return 1;
}

void CommandSync::SendPhase(const Entry& entry, CommandPhase phase) {
    if (!send_) {
        return;
    }
    CommandMessage msg;
    msg.sender_id = sender_id_;
    msg.sequence = next_sequence_++;
    msg.phase = phase;
    msg.name = entry.spec.name;
    send_(EncodeCommandMessage(msg));
}

void CommandSync::Ingest(const uint8_t* data, size_t len, double now_s) {
    const auto msg = DecodeCommandMessage(data, len);
    if (!msg || msg->sender_id == sender_id_ || !dedup_.Accept(msg->sender_id, msg->sequence)) {
        return;
    }
    const auto it = by_name_.find(msg->name);
    if (it == by_name_.end()) {
        return; // not in our profile - never executed on the peer's behalf
    }
    replaying_ = true;
    if (msg->phase == CommandPhase::kBegin) {
        if (!remote_held_.count(msg->name)) {
            XPLMCommandBegin(it->second->ref);
        }
        remote_held_[msg->name] = now_s;
    } else if (remote_held_.erase(msg->name)) {
        XPLMCommandEnd(it->second->ref);
    }
    replaying_ = false;
}

void CommandSync::Poll(double now_s) {
    for (auto it = remote_held_.begin(); it != remote_held_.end();) {
        if (now_s - it->second > kMaxRemoteHoldS) {
            const auto entry = by_name_.find(it->first);
            if (entry != by_name_.end()) {
                replaying_ = true;
                XPLMCommandEnd(entry->second->ref);
                replaying_ = false;
            }
            it = remote_held_.erase(it);
        } else {
            ++it;
        }
    }
}

} // namespace flytogether
