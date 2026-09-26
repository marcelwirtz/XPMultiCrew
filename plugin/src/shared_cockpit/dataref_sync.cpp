#include "shared_cockpit/dataref_sync.h"

#include <algorithm>
#include <optional>

namespace flytogether {

bool DatarefSync::Start(const std::vector<DatarefSyncSpec>& watched, const std::vector<Peer>& peers,
                         bool seedFromCurrentValues, bool startsOwningAllCategories) {
    peers_ = peers;
    watched_.clear();
    index_by_name_.clear();
    ownership_ = OwnershipTracker(startsOwningAllCategories);

    for (const auto& spec : watched) {
        XPLMDataRef ref = XPLMFindDataRef(spec.name.c_str());
        if (!ref) {
            continue; // silently skip unknown datarefs, same as elsewhere in this plugin
        }
        WatchedDataref w;
        w.name = spec.name;
        w.ref = ref;
        w.xplm_type = XPLMGetDataRefTypes(ref);
        w.stream = spec.stream;
        w.category = spec.category;
        if (seedFromCurrentValues) {
            w.last_known = ReadCurrentValue(w);
            w.has_last_known = true;
        }
        watched_.push_back(w);
        index_by_name_[spec.name] = watched_.size() - 1;
    }

    if (!socket_.Open()) {
        return false;
    }
    if (!socket_.Bind(kDatarefSyncUdpPort)) {
        // Closed rather than left open-but-unbound: that socket is still in
        // blocking mode, so a later ReceiveFrom() would hang the sim.
        socket_.Close();
        return false;
    }
    socket_.SetNonBlocking(true);
    return true;
}

void DatarefSync::Stop() {
    socket_.Close();
    peers_.clear();
    watched_.clear();
    index_by_name_.clear();
}

DatarefValue DatarefSync::ReadCurrentValue(const WatchedDataref& w) const {
    DatarefValue v;
    if (w.xplm_type & xplmType_Int) {
        v.type = DatarefValueType::kInt;
        v.int_value = XPLMGetDatai(w.ref);
    } else if (w.xplm_type & xplmType_Double) {
        v.type = DatarefValueType::kDouble;
        v.double_value = XPLMGetDatad(w.ref);
    } else if (w.xplm_type & xplmType_Float) {
        v.type = DatarefValueType::kFloat;
        v.float_value = XPLMGetDataf(w.ref);
    } else if (w.xplm_type & xplmType_IntArray) {
        v.type = DatarefValueType::kIntArray;
        int count = XPLMGetDatavi(w.ref, nullptr, 0, 0);
        count = std::min(count, kDatarefSyncMaxArrayLen);
        if (count > 0) {
            XPLMGetDatavi(w.ref, v.int_array, 0, count);
        }
        v.array_len = count;
    } else if (w.xplm_type & xplmType_FloatArray) {
        v.type = DatarefValueType::kFloatArray;
        int count = XPLMGetDatavf(w.ref, nullptr, 0, 0);
        count = std::min(count, kDatarefSyncMaxArrayLen);
        if (count > 0) {
            XPLMGetDatavf(w.ref, v.float_array, 0, count);
        }
        v.array_len = count;
    }
    return v;
}

void DatarefSync::ApplyValue(WatchedDataref& w, const DatarefValue& value) {
    switch (value.type) {
        case DatarefValueType::kInt:
            XPLMSetDatai(w.ref, value.int_value);
            break;
        case DatarefValueType::kFloat:
            XPLMSetDataf(w.ref, value.float_value);
            break;
        case DatarefValueType::kDouble:
            XPLMSetDatad(w.ref, value.double_value);
            break;
        case DatarefValueType::kIntArray: {
            // value.array_len comes straight off the network (peer-supplied,
            // clamped only to kDatarefSyncMaxArrayLen by the decoder) - clamp
            // it again to this specific dataref's actual element count before
            // handing it to XPLM, so a peer/protocol mismatch (e.g. a stale
            // profile watching a dataref that's a shorter array on this
            // side) can't make XPLMSetDatavi write past the real array.
            const int real_len = XPLMGetDatavi(w.ref, nullptr, 0, 0);
            const int count = std::min(value.array_len, real_len);
            if (count > 0) {
                XPLMSetDatavi(w.ref, const_cast<int*>(value.int_array), 0, count);
            }
            break;
        }
        case DatarefValueType::kFloatArray: {
            const int real_len = XPLMGetDatavf(w.ref, nullptr, 0, 0);
            const int count = std::min(value.array_len, real_len);
            if (count > 0) {
                XPLMSetDatavf(w.ref, const_cast<float*>(value.float_array), 0, count);
            }
            break;
        }
    }
    // Cache the value we just applied so the next Poll() doesn't see it as
    // a new local change and bounce it right back to whoever sent it.
    w.last_known = value;
    w.has_last_known = true;
}

void DatarefSync::Poll() {
    // 1. Detect local changes, claiming their category first if this side
    // doesn't already own it (see this class's comment), then broadcast
    // the new value unconditionally - touching it IS taking it. A dataref
    // seen for the very first time (no last_known yet) is always adopted
    // as the initial cached baseline (nothing to compare against), and
    // still goes through the same claim-then-broadcast path as any other
    // change - a cold-start value from an unowned category becomes this
    // side's own claimed value, same as a real switch flip would.
    for (auto& w : watched_) {
        const DatarefValue current = ReadCurrentValue(w);
        const bool changed = w.stream || !w.has_last_known || current != w.last_known;
        if (!changed) {
            continue;
        }

        w.last_known = current;
        w.has_last_known = true;

        if (!ownership_.Owns(w.category)) {
            ClaimOwnership(w.category);
        }
        const auto encoded = EncodeDatarefSyncMessage(DatarefSyncMessage{w.name, current});
        if (!encoded.empty()) {
            SendToPeers(encoded);
        }
    }

    // 2. Apply anything a peer changed. Runs after step 1 above, so a
    // remote change applied this tick can't be mistaken for a local one
    // until the *next* Poll() call - and by then w.last_known already
    // matches, so it won't be re-broadcast either.
    uint8_t buf[512];
    while (true) {
        const int received = socket_.ReceiveFrom(buf, sizeof(buf));
        if (received < 0) {
            break;
        }

        // Decrypted here for the direct-UDP path only - see
        // IngestRelayedMessage's comment for why the relay path is
        // different.
        const uint8_t* plain_data = buf;
        size_t plain_size = static_cast<size_t>(received);
        std::optional<std::vector<uint8_t>> opened;
        if (crypto_) {
            opened = crypto_->Open(buf, static_cast<size_t>(received));
            if (!opened) {
                continue; // wrong/no key, or corrupted/foreign packet
            }
            plain_data = opened->data();
            plain_size = opened->size();
        }
        ApplyIncomingBytes(plain_data, plain_size);
    }
}

void DatarefSync::IngestRelayedMessage(const void* data, size_t len) {
    // Already PLAINTEXT, unlike the direct-UDP path above - NOT decrypted
    // again here. The relay channel multiplexes several message shapes
    // (position, dataref sync, ownership, weather) onto one stream, and
    // plugin_main.cpp's relay dispatcher has to decrypt once before it
    // can even peek the magic byte to know which Ingest* to call in the
    // first place - see that dispatcher's comment. A second Open() here
    // on already-decrypted bytes would just fail.
    ApplyIncomingBytes(data, len);
}

void DatarefSync::SendToPeers(const std::vector<uint8_t>& encoded) {
    // Every message shape sharing this channel (dataref value changes,
    // ownership requests/responses) funnels through here, so sealing it
    // once at this single choke point covers all of them - see
    // SetCrypto's comment.
    if (crypto_) {
        const auto envelope = crypto_->Seal(encoded);
        for (const auto& peer : peers_) {
            socket_.SendTo(peer.host, peer.port, envelope.data(), envelope.size());
        }
        if (relay_sender_) {
            relay_sender_(envelope.data(), envelope.size());
        }
        return;
    }
    for (const auto& peer : peers_) {
        socket_.SendTo(peer.host, peer.port, encoded.data(), encoded.size());
    }
    if (relay_sender_) {
        relay_sender_(encoded.data(), encoded.size());
    }
}

void DatarefSync::ClaimOwnership(DatarefCategory category) {
    const auto msg = ownership_.Claim(category);
    if (!msg) {
        return; // already own it - nothing to send
    }
    const auto encoded = EncodeOwnershipClaimMessage(*msg);
    if (encoded.empty()) {
        return;
    }
    // Sent a few times back-to-back: this channel is plain best-effort UDP
    // with no ACK, and a one-way notify has nothing to resend on beyond
    // that - unlike the old request/grant handshake, there's no reply to
    // wait for and so no pending state to dedupe a repeat send against.
    for (int i = 0; i < 3; ++i) {
        SendToPeers(encoded);
    }
}

void DatarefSync::ApplyIncomingBytes(const void* data, size_t len) {
    // `data`/`len` are always already PLAINTEXT by the time they reach
    // here - decrypted by Poll() for the direct-UDP path, or by
    // plugin_main.cpp's relay dispatcher for the relay path (see
    // IngestRelayedMessage's comment). No decryption happens in this
    // function itself.
    const auto* bytes = static_cast<const uint8_t*>(data);
    const uint32_t magic = PeekDatarefSyncChannelMagic(bytes, len);

    if (magic == kOwnershipClaimMagic) {
        OwnershipClaimMessage claim;
        if (DecodeOwnershipClaimMessage(bytes, len, claim)) {
            ownership_.OnPeerClaimed(claim.category);
        }
        return;
    }

    DatarefSyncMessage msg;
    if (!DecodeDatarefSyncMessage(bytes, len, msg)) {
        return;
    }

    const auto it = index_by_name_.find(msg.name);
    if (it == index_by_name_.end()) {
        return; // not a dataref we're watching
    }
    WatchedDataref& w = watched_[it->second];
    if (!ownership_.ShouldApplyRemoteWrite(w.category)) {
        return; // this side owns the category - protect it from a stale/racing remote write
    }
    ApplyValue(w, msg.value);
}

} // namespace flytogether
