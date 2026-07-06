#include "codec/market_tick_codec.hpp"
#include "bus/message_protocol.hpp"

#include <cassert>
#include <cstring>

int main() {
    MarketTick tick{};
    tick.timestamp_ns = 1700000000000000000ULL;
    tick.price = 50000.25;
    tick.quantity = 0.42;
    std::strncpy(tick.symbol, "BTC/USDT", sizeof(tick.symbol) - 1);
    std::strncpy(tick.source, "BINANCE", sizeof(tick.source) - 1);
    tick.side = SIDE_BUY;

    std::vector<uint8_t> payload;
    assert(argentum::codec::encode_market_tick_legacy(tick, &payload) == ARGENTUM_OK);
    assert(!payload.empty());

    MarketTick decoded{};
    assert(argentum::codec::decode_market_tick(payload.data(), payload.size(), &decoded) == ARGENTUM_OK);
    assert(decoded.timestamp_ns == tick.timestamp_ns);
    assert(decoded.price == tick.price);
    assert(decoded.quantity == tick.quantity);
    assert(std::strcmp(decoded.symbol, tick.symbol) == 0);
    assert(std::strcmp(decoded.source, tick.source) == 0);
    assert(decoded.side == tick.side);

    assert(argentum::codec::decode_market_tick(payload.data(), 4, &decoded) == ARGENTUM_ERR_PROTO);

    // CRC validation (V2)
    const uint8_t raw_payload[4] = {1, 2, 3, 4};
    auto msg_v2 = argentum::bus::encode_message_v2(
        argentum::bus::MessageType::MarketTick,
        raw_payload,
        sizeof(raw_payload),
        1234,
        static_cast<uint32_t>(argentum::bus::MessageFlags::HasCrc32));

    argentum::bus::DecodedHeader hdr{};
    assert(argentum::bus::decode_header(msg_v2.data(), msg_v2.size(), &hdr) == ARGENTUM_OK);

    // Corrupt payload and expect CRC failure.
    msg_v2.back() ^= 0xFF;
    assert(argentum::bus::decode_header(msg_v2.data(), msg_v2.size(), &hdr) == ARGENTUM_ERR_PROTO);

    // Invalid size should fail.
    argentum::bus::MessageHeaderV1 bad_hdr{};
    bad_hdr.version = argentum::bus::kMessageProtocolVersionV1;
    bad_hdr.type = static_cast<uint16_t>(argentum::bus::MessageType::MarketTick);
    bad_hdr.size = 1024;
    bad_hdr.timestamp_ns = 0;
    std::vector<uint8_t> bad_buf(sizeof(bad_hdr));
    std::memcpy(bad_buf.data(), &bad_hdr, sizeof(bad_hdr));
    assert(argentum::bus::decode_header(bad_buf.data(), bad_buf.size(), &hdr) == ARGENTUM_ERR_PROTO);

    return 0;
}
