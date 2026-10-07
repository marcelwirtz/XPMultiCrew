#include "shared_cockpit/dataref_sync.h"

#include <algorithm>
#include <cstdio>
#include <optional>

namespace flytogether {

bool DatarefSync::Start(const std::vector<DatarefSyncSpec>& watched, const std::vector<Peer>& peers,
                         bool isMaster) {
    peers_ = peers;
    watched_.clear();
    index_by_name_.clear();
    // Only the "flight" category means anything any more (who flies); the
    // master starts with it.
    ownership_ = OwnershipTracker(isMaster);
    is_master_ = isMaster;
    next_refresh_s_ = 0.0;

    for (const auto& spec : watched) {
        XPLMDataRef ref = XPLMFindDataRef(spec.name.c_str());
        if (!ref || index_by_name_.count(spec.name)) {
            continue; // silently skip unknown datarefs, same as elsewhere in this plugin
        }
        WatchedDataref w;
        w.name = spec.name;
        w.ref = ref;
        w.xplm_type = XPLMGetDataRefTypes(ref);
        w.state.stream = spec.stream;
        w.state.output = spec.output;
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
    MarkApplied(w.state, value);
}

void DatarefSync::SetAuthority(bool isMaster) {
    if (isMaster == is_master_) {
        return;
    }
    is_master_ = isMaster;
    for (auto& w : watched_) {
        w.state.has_master_value = false;
        w.state.hold_local_until = 0.0;
    }
    // A new master sends everything once, so both cockpits start its
    // stint from the same state.
    next_refresh_s_ = 0.0;
}

void DatarefSync::Poll(double now_s) {
    // 1. Send what changed here (see sync_policy.h for who sends what).
    // The master also re-sends everything every kRefreshIntervalS, which
    // heals a lost packet or a co-pilot that joined late.
    constexpr double kRefreshIntervalS = 10.0;
    const bool refresh = is_master_ && now_s >= next_refresh_s_;
    if (refresh) {
        next_refresh_s_ = now_s + kRefreshIntervalS;
    }
    for (auto& w : watched_) {
        const DatarefValue current = ReadCurrentValue(w);
        switch (OnLocalValue(w.state, current, is_master_, refresh, now_s)) {
            case LocalAction::kNone:
                break;
            case LocalAction::kSend: {
                const auto encoded = EncodeDatarefSyncMessage(DatarefSyncMessage{w.name, current});
                if (!encoded.empty()) {
                    SendToPeers(encoded);
                }
                break;
            }
            case LocalAction::kWriteMaster:
                ApplyValue(w, w.state.master_value);
                break;
        }
    }

    // 2. Apply what the peer sent. Runs after step 1 above, so a value
    // applied this tick is already last_known by the next Poll() and isn't
    // mistaken for a local change.
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
        ApplyIncomingBytes(plain_data, plain_size, now_s);
    }
}

void DatarefSync::IngestRelayedMessage(const void* data, size_t len, double now_s) {
    // Already PLAINTEXT, unlike the direct-UDP path above - NOT decrypted
    // again here. The relay channel multiplexes several message shapes
    // (position, dataref sync, ownership, weather) onto one stream, and
    // plugin_main.cpp's relay dispatcher has to decrypt once before it
    // can even peek the magic byte to know which Ingest* to call in the
    // first place - see that dispatcher's comment. A second Open() here
    // on already-decrypted bytes would just fail.
    ApplyIncomingBytes(data, len, now_s);
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

namespace {
uint32_t Fnv1a(const void* data, size_t len, uint32_t h = 2166136261u) {
    const auto* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < len; ++i) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

uint32_t HashValue(const DatarefValue& v) {
    uint32_t h = Fnv1a(&v.type, sizeof(v.type));
    switch (v.type) {
        case DatarefValueType::kInt: return Fnv1a(&v.int_value, sizeof(v.int_value), h);
        case DatarefValueType::kFloat: return Fnv1a(&v.float_value, sizeof(v.float_value), h);
        case DatarefValueType::kDouble: return Fnv1a(&v.double_value, sizeof(v.double_value), h);
        case DatarefValueType::kIntArray: return Fnv1a(v.int_array, sizeof(int) * static_cast<size_t>(v.array_len), h);
        case DatarefValueType::kFloatArray:
            return Fnv1a(v.float_array, sizeof(float) * static_cast<size_t>(v.array_len), h);
    }
    return h;
}
} // namespace

std::vector<uint32_t> DatarefSync::ValueHashes() const {
    std::vector<uint32_t> out;
    out.reserve(watched_.size());
    for (const auto& w : watched_) {
        out.push_back(HashValue(ReadCurrentValue(w)));
    }
    return out;
}

uint32_t DatarefSync::ProfileHash() const {
    uint32_t h = 2166136261u;
    for (const auto& w : watched_) {
        h = Fnv1a(w.name.data(), w.name.size(), h);
        h = Fnv1a("\n", 1, h);
    }
    return h;
}

std::string DatarefSync::DescribeCurrentValue(size_t index) const {
    const DatarefValue v = ReadCurrentValue(watched_[index]);
    char buf[32];
    switch (v.type) {
        case DatarefValueType::kInt: std::snprintf(buf, sizeof(buf), "%d", v.int_value); break;
        case DatarefValueType::kFloat: std::snprintf(buf, sizeof(buf), "%.6g", v.float_value); break;
        case DatarefValueType::kDouble: std::snprintf(buf, sizeof(buf), "%.6g", v.double_value); break;
        // Arrays: first element only - enough to recognise the mismatch.
        case DatarefValueType::kIntArray:
            std::snprintf(buf, sizeof(buf), "[0]=%d", v.array_len ? v.int_array[0] : 0);
            break;
        case DatarefValueType::kFloatArray:
            std::snprintf(buf, sizeof(buf), "[0]=%.6g", v.array_len ? v.float_array[0] : 0.0f);
            break;
    }
    return buf;
}

void DatarefSync::ResendAll() {
    if (is_master_) {
        next_refresh_s_ = 0.0; // the next Poll() sends everything
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

void DatarefSync::ApplyIncomingBytes(const void* data, size_t len, double now_s) {
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
    if (OnRemoteValue(w.state, msg.value, is_master_, now_s)) {
        ApplyValue(w, msg.value);
    }
}

} // namespace flytogether
