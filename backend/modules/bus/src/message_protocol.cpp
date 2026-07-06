#include "bus/message_protocol.hpp"

#include <cstring>
#include <mutex>

namespace argentum::bus {

static uint32_t crc32_table[256];
static std::once_flag crc32_once;

static void init_crc32_table() {
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (uint32_t j = 0; j < 8; ++j) {
            c = (c & 1) ? (0xEDB88320U ^ (c >> 1)) : (c >> 1);
        }
        crc32_table[i] = c;
    }
}

uint32_t compute_crc32(const uint8_t* data, size_t size) {
    if (!data || size == 0) return 0;
    std::call_once(crc32_once, init_crc32_table);
    uint32_t c = 0xFFFFFFFFU;
    for (size_t i = 0; i < size; ++i) {
        c = crc32_table[(c ^ data[i]) & 0xFFU] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFFU;
}

std::vector<uint8_t> encode_message(MessageType type, const void* data, size_t size, uint64_t timestamp_ns) {
    std::vector<uint8_t> buffer(sizeof(MessageHeaderV1) + size);
    MessageHeaderV1 header{};
    header.version = kMessageProtocolVersionV1;
    header.type = static_cast<uint16_t>(type);
    header.size = static_cast<uint32_t>(size);
    header.timestamp_ns = timestamp_ns;
    std::memcpy(buffer.data(), &header, sizeof(header));
    if (size > 0 && data) {
        std::memcpy(buffer.data() + sizeof(header), data, size);
    }
    return buffer;
}

std::vector<uint8_t> encode_message_v2(MessageType type, const void* data, size_t size, uint64_t timestamp_ns, uint32_t flags) {
    std::vector<uint8_t> buffer(sizeof(MessageHeaderV2) + size);
    MessageHeaderV2 header{};
    header.version = kMessageProtocolVersionV2;
    header.type = static_cast<uint16_t>(type);
    header.size = static_cast<uint32_t>(size);
    header.timestamp_ns = timestamp_ns;
    header.flags = flags;
    header.crc32 = (flags & static_cast<uint32_t>(MessageFlags::HasCrc32)) ? compute_crc32(static_cast<const uint8_t*>(data), size) : 0;
    std::memcpy(buffer.data(), &header, sizeof(header));
    if (size > 0 && data) {
        std::memcpy(buffer.data() + sizeof(header), data, size);
    }
    return buffer;
}

ArgentumStatus decode_header(const void* data, size_t size, DecodedHeader* out_header) {
    if (!data || !out_header) return ARGENTUM_ERR_INVALID;
    if (size < sizeof(MessageHeaderV1)) return ARGENTUM_ERR_PROTO;

    uint16_t version = 0;
    std::memcpy(&version, data, sizeof(version));

    if (version == kMessageProtocolVersionV1) {
        if (size < sizeof(MessageHeaderV1)) return ARGENTUM_ERR_PROTO;
        MessageHeaderV1 header{};
        std::memcpy(&header, data, sizeof(header));
        out_header->header = MessageHeader{
            header.version,
            header.type,
            header.size,
            header.timestamp_ns,
            0,
            0
        };
        out_header->header_size = sizeof(MessageHeaderV1);
    } else if (version == kMessageProtocolVersionV2) {
        if (size < sizeof(MessageHeaderV2)) return ARGENTUM_ERR_PROTO;
        MessageHeaderV2 header{};
        std::memcpy(&header, data, sizeof(header));
        out_header->header = MessageHeader{
            header.version,
            header.type,
            header.size,
            header.timestamp_ns,
            header.flags,
            header.crc32
        };
        out_header->header_size = sizeof(MessageHeaderV2);
    } else {
        return ARGENTUM_ERR_PROTO;
    }

    if (out_header->header.size > size - out_header->header_size) return ARGENTUM_ERR_PROTO;

    if (out_header->header.flags & static_cast<uint32_t>(MessageFlags::HasCrc32)) {
        const uint8_t* payload = static_cast<const uint8_t*>(data) + out_header->header_size;
        uint32_t crc = compute_crc32(payload, out_header->header.size);
        if (crc != out_header->header.crc32) return ARGENTUM_ERR_PROTO;
    }

    return ARGENTUM_OK;
}

const uint8_t* payload_ptr(const void* data, size_t size, size_t header_size) {
    if (!data || size < header_size) return nullptr;
    return static_cast<const uint8_t*>(data) + header_size;
}

} // namespace argentum::bus
