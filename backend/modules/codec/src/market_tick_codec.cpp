#include "codec/market_tick_codec.hpp"

#include "bus/message_protocol.hpp"

#include <cstring>

#ifdef ARGENTUM_USE_FLATBUFFERS
#include "argentum_generated.h"
#include <flatbuffers/flatbuffers.h>
#include <flatbuffers/verifier.h>
#endif

namespace argentum::codec {

ArgentumStatus encode_market_tick_legacy(const MarketTick& tick, std::vector<uint8_t>* out) {
    if (!out) return ARGENTUM_ERR_INVALID;
    *out = bus::encode_message(bus::MessageType::MarketTick, &tick, sizeof(tick), tick.timestamp_ns);
    return ARGENTUM_OK;
}

#ifdef ARGENTUM_USE_FLATBUFFERS
ArgentumStatus encode_market_tick_flatbuffers(const MarketTick& tick, std::vector<uint8_t>* out, bool with_crc) {
    if (!out) return ARGENTUM_ERR_INVALID;

    flatbuffers::FlatBufferBuilder builder(128);
    auto symbol = builder.CreateString(tick.symbol);
    auto source = builder.CreateString(tick.source);
    auto tick_fb = argentum::CreateMarketTick(builder,
                                              tick.timestamp_ns,
                                              tick.price,
                                              tick.quantity,
                                              symbol,
                                              source,
                                              static_cast<argentum::Side>(tick.side));
    builder.Finish(tick_fb);

    uint32_t flags = with_crc ? static_cast<uint32_t>(bus::MessageFlags::HasCrc32) : 0;
    *out = bus::encode_message_v2(bus::MessageType::MarketTick,
                                  builder.GetBufferPointer(),
                                  builder.GetSize(),
                                  tick.timestamp_ns,
                                  flags);
    return ARGENTUM_OK;
}
#endif

ArgentumStatus decode_market_tick(const void* data, size_t size, MarketTick* out) {
    if (!data || !out) return ARGENTUM_ERR_INVALID;

    bus::DecodedHeader decoded{};
    ArgentumStatus status = bus::decode_header(data, size, &decoded);
    if (status != ARGENTUM_OK) return status;

    if (decoded.header.type != static_cast<uint16_t>(bus::MessageType::MarketTick)) {
        return ARGENTUM_ERR_PROTO;
    }

    const uint8_t* payload = bus::payload_ptr(data, size, decoded.header_size);
    if (!payload) return ARGENTUM_ERR_PROTO;

#ifdef ARGENTUM_USE_FLATBUFFERS
    if (decoded.header.version == bus::kMessageProtocolVersionV2) {
        flatbuffers::Verifier verifier(payload, decoded.header.size);
        if (!verifier.VerifyBuffer<argentum::MarketTick>(nullptr)) return ARGENTUM_ERR_PROTO;
        const auto* tick_fb = flatbuffers::GetRoot<argentum::MarketTick>(payload);
        if (!tick_fb) return ARGENTUM_ERR_PROTO;

        out->timestamp_ns = tick_fb->timestamp_ns();
        out->price = tick_fb->price();
        out->quantity = tick_fb->quantity();

        std::memset(out->symbol, 0, sizeof(out->symbol));
        std::memset(out->source, 0, sizeof(out->source));

        if (const auto* symbol = tick_fb->symbol()) {
            std::strncpy(out->symbol, symbol->c_str(), sizeof(out->symbol) - 1);
        }
        if (const auto* source = tick_fb->source()) {
            std::strncpy(out->source, source->c_str(), sizeof(out->source) - 1);
        }

        out->side = static_cast<uint8_t>(tick_fb->side());
        return ARGENTUM_OK;
    }
#endif

    if (decoded.header.version != bus::kMessageProtocolVersionV1) {
        return ARGENTUM_ERR_PROTO;
    }

    if (decoded.header.size != sizeof(MarketTick)) {
        return ARGENTUM_ERR_PROTO;
    }

    std::memcpy(out, payload, sizeof(MarketTick));
    return ARGENTUM_OK;
}

} // namespace argentum::codec
