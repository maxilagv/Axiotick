#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "core/errors.h"

namespace argentum::bus {

constexpr uint16_t kMessageProtocolVersionV1 = 1;
constexpr uint16_t kMessageProtocolVersionV2 = 2;

enum class MessageType : uint16_t {
    MarketTick = 1,
    Order = 2,
    Trade = 3
};

enum class MessageFlags : uint32_t {
    None = 0,
    HasCrc32 = 1 << 0
};

struct MessageHeaderV1 {
    uint16_t version;
    uint16_t type;
    uint32_t size;
    uint64_t timestamp_ns;
};

struct MessageHeaderV2 {
    uint16_t version;
    uint16_t type;
    uint32_t size;
    uint64_t timestamp_ns;
    uint32_t flags;
    uint32_t crc32;
};

#ifdef __cplusplus
static_assert(sizeof(MessageHeaderV1) == 16, "MessageHeaderV1 must be 16 bytes.");
static_assert(sizeof(MessageHeaderV2) == 24, "MessageHeaderV2 must be 24 bytes.");
#endif

struct MessageHeader {
    uint16_t version;
    uint16_t type;
    uint32_t size;
    uint64_t timestamp_ns;
    uint32_t flags;
    uint32_t crc32;
};

struct DecodedHeader {
    MessageHeader header;
    size_t header_size;
};

std::vector<uint8_t> encode_message(MessageType type, const void* data, size_t size, uint64_t timestamp_ns);
std::vector<uint8_t> encode_message_v2(MessageType type, const void* data, size_t size, uint64_t timestamp_ns, uint32_t flags);
ArgentumStatus decode_header(const void* data, size_t size, DecodedHeader* out_header);
const uint8_t* payload_ptr(const void* data, size_t size, size_t header_size);
uint32_t compute_crc32(const uint8_t* data, size_t size);

} // namespace argentum::bus
