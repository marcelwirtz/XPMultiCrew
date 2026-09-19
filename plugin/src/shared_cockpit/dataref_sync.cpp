#include "shared_cockpit/dataref_sync.h"

#include <algorithm>

namespace flytogether {

bool DatarefSync::Start(const std::vector<std::string>& watchedNames, const std::vector<Peer>& peers) {
    peers_ = peers;
    watched_.clear();
    index_by_name_.clear();

    for (const auto& name : watchedNames) {
        XPLMDataRef ref = XPLMFindDataRef(name.c_str());
        if (!ref) {
            continue; // silently skip unknown datarefs, same as elsewhere in this plugin
        }
        WatchedDataref w;
        w.name = name;
        w.ref = ref;
        w.xplm_type = XPLMGetDataRefTypes(ref);
        watched_.push_back(w);
        index_by_name_[name] = watched_.size() - 1;
    }

    if (!socket_.Open()) {
        return false;
    }
    if (!socket_.Bind(kDatarefSyncUdpPort)) {
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
        case DatarefValueType::kIntArray:
            XPLMSetDatavi(w.ref, const_cast<int*>(value.int_array), 0, value.array_len);
            break;
        case DatarefValueType::kFloatArray:
            XPLMSetDatavf(w.ref, const_cast<float*>(value.float_array), 0, value.array_len);
            break;
    }
    // Cache the value we just applied so the next Poll() doesn't see it as
    // a new local change and bounce it right back to whoever sent it.
    w.last_known = value;
    w.has_last_known = true;
}

void DatarefSync::Poll() {
    // 1. Detect and broadcast local changes.
    for (auto& w : watched_) {
        const DatarefValue current = ReadCurrentValue(w);
        if (!w.has_last_known || current != w.last_known) {
            w.last_known = current;
            w.has_last_known = true;

            const auto encoded = EncodeDatarefSyncMessage(DatarefSyncMessage{w.name, current});
            if (!encoded.empty()) {
                for (const auto& peer : peers_) {
                    socket_.SendTo(peer.host, peer.port, encoded.data(), encoded.size());
                }
                if (relay_sender_) {
                    relay_sender_(encoded.data(), encoded.size());
                }
            }
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
        ApplyIncomingBytes(buf, static_cast<size_t>(received));
    }
}

void DatarefSync::IngestRelayedMessage(const void* data, size_t len) {
    ApplyIncomingBytes(data, len);
}

void DatarefSync::ApplyIncomingBytes(const void* data, size_t len) {
    DatarefSyncMessage msg;
    if (!DecodeDatarefSyncMessage(static_cast<const uint8_t*>(data), len, msg)) {
        return;
    }

    const auto it = index_by_name_.find(msg.name);
    if (it == index_by_name_.end()) {
        return; // not a dataref we're watching
    }
    ApplyValue(watched_[it->second], msg.value);
}

} // namespace flytogether
