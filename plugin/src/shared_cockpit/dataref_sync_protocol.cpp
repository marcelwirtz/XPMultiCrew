#include "shared_cockpit/dataref_sync_protocol.h"

#include <cstring>

namespace flytogether {

bool DatarefValue::operator==(const DatarefValue& other) const {
    if (type != other.type) {
        return false;
    }
    switch (type) {
        case DatarefValueType::kInt:
            return int_value == other.int_value;
        case DatarefValueType::kFloat:
            return float_value == other.float_value;
        case DatarefValueType::kDouble:
            return double_value == other.double_value;
        case DatarefValueType::kIntArray:
            if (array_len != other.array_len) return false;
            for (int i = 0; i < array_len; ++i) {
                if (int_array[i] != other.int_array[i]) return false;
            }
            return true;
        case DatarefValueType::kFloatArray:
            if (array_len != other.array_len) return false;
            for (int i = 0; i < array_len; ++i) {
                if (float_array[i] != other.float_array[i]) return false;
            }
            return true;
    }
    return false;
}

namespace {
void Append(std::vector<uint8_t>& out, const void* p, size_t n) {
    const auto* b = static_cast<const uint8_t*>(p);
    out.insert(out.end(), b, b + n);
}
} // namespace

uint32_t PeekDatarefSyncChannelMagic(const uint8_t* data, size_t len) {
    if (len < sizeof(uint32_t)) {
        return 0;
    }
    uint32_t magic = 0;
    std::memcpy(&magic, data, sizeof(magic));
    return magic;
}

std::vector<uint8_t> EncodeDatarefSyncMessage(const DatarefSyncMessage& msg) {
    if (msg.name.size() > 0xFFFF) {
        return {};
    }
    if (msg.value.array_len < 0 || msg.value.array_len > kDatarefSyncMaxArrayLen) {
        return {};
    }

    std::vector<uint8_t> out;
    const uint32_t magic = kDatarefSyncMagic;
    const uint32_t version = kDatarefSyncProtocolVersion;
    const uint16_t name_len = static_cast<uint16_t>(msg.name.size());
    const uint8_t type = static_cast<uint8_t>(msg.value.type);
    const uint8_t array_len = static_cast<uint8_t>(msg.value.array_len);

    Append(out, &magic, sizeof(magic));
    Append(out, &version, sizeof(version));
    Append(out, &name_len, sizeof(name_len));
    Append(out, &type, sizeof(type));
    Append(out, &array_len, sizeof(array_len));
    Append(out, msg.name.data(), msg.name.size());

    switch (msg.value.type) {
        case DatarefValueType::kInt:
            Append(out, &msg.value.int_value, sizeof(msg.value.int_value));
            break;
        case DatarefValueType::kFloat:
            Append(out, &msg.value.float_value, sizeof(msg.value.float_value));
            break;
        case DatarefValueType::kDouble:
            Append(out, &msg.value.double_value, sizeof(msg.value.double_value));
            break;
        case DatarefValueType::kIntArray:
            Append(out, msg.value.int_array, sizeof(int) * static_cast<size_t>(array_len));
            break;
        case DatarefValueType::kFloatArray:
            Append(out, msg.value.float_array, sizeof(float) * static_cast<size_t>(array_len));
            break;
    }

    return out;
}

bool DecodeDatarefSyncMessage(const uint8_t* data, size_t len, DatarefSyncMessage& out) {
    constexpr size_t kHeaderLen = sizeof(uint32_t) * 2 + sizeof(uint16_t) + 1 + 1;
    if (len < kHeaderLen) {
        return false;
    }

    uint32_t magic = 0, version = 0;
    uint16_t name_len = 0;
    uint8_t type_byte = 0, array_len = 0;

    size_t pos = 0;
    std::memcpy(&magic, data + pos, sizeof(magic)); pos += sizeof(magic);
    std::memcpy(&version, data + pos, sizeof(version)); pos += sizeof(version);
    std::memcpy(&name_len, data + pos, sizeof(name_len)); pos += sizeof(name_len);
    std::memcpy(&type_byte, data + pos, sizeof(type_byte)); pos += sizeof(type_byte);
    std::memcpy(&array_len, data + pos, sizeof(array_len)); pos += sizeof(array_len);

    if (magic != kDatarefSyncMagic || version != kDatarefSyncProtocolVersion) {
        return false;
    }
    if (type_byte > static_cast<uint8_t>(DatarefValueType::kFloatArray)) {
        return false;
    }
    if (array_len > kDatarefSyncMaxArrayLen) {
        return false;
    }
    if (len < pos + name_len) {
        return false;
    }

    out.name.assign(reinterpret_cast<const char*>(data + pos), name_len);
    pos += name_len;

    out.value = DatarefValue{};
    out.value.type = static_cast<DatarefValueType>(type_byte);
    out.value.array_len = array_len;

    switch (out.value.type) {
        case DatarefValueType::kInt:
            if (len < pos + sizeof(out.value.int_value)) return false;
            std::memcpy(&out.value.int_value, data + pos, sizeof(out.value.int_value));
            break;
        case DatarefValueType::kFloat:
            if (len < pos + sizeof(out.value.float_value)) return false;
            std::memcpy(&out.value.float_value, data + pos, sizeof(out.value.float_value));
            break;
        case DatarefValueType::kDouble:
            if (len < pos + sizeof(out.value.double_value)) return false;
            std::memcpy(&out.value.double_value, data + pos, sizeof(out.value.double_value));
            break;
        case DatarefValueType::kIntArray: {
            const size_t bytes = sizeof(int) * static_cast<size_t>(array_len);
            if (len < pos + bytes) return false;
            std::memcpy(out.value.int_array, data + pos, bytes);
            break;
        }
        case DatarefValueType::kFloatArray: {
            const size_t bytes = sizeof(float) * static_cast<size_t>(array_len);
            if (len < pos + bytes) return false;
            std::memcpy(out.value.float_array, data + pos, bytes);
            break;
        }
    }

    return true;
}

std::vector<uint8_t> EncodeOwnershipClaimMessage(const OwnershipClaimMessage& msg) {
    std::vector<uint8_t> out;
    const uint32_t magic = kOwnershipClaimMagic;
    const uint8_t category = static_cast<uint8_t>(msg.category);
    Append(out, &magic, sizeof(magic));
    Append(out, &category, sizeof(category));
    return out;
}

bool DecodeOwnershipClaimMessage(const uint8_t* data, size_t len, OwnershipClaimMessage& out) {
    constexpr size_t kExpectedLen = sizeof(uint32_t) + 1;
    if (len < kExpectedLen) {
        return false;
    }
    uint32_t magic = 0;
    std::memcpy(&magic, data, sizeof(magic));
    if (magic != kOwnershipClaimMagic) {
        return false;
    }
    const uint8_t category_byte = data[sizeof(magic)];
    if (category_byte >= kDatarefCategoryCount) {
        return false;
    }
    out.category = static_cast<DatarefCategory>(category_byte);
    return true;
}

} // namespace flytogether
