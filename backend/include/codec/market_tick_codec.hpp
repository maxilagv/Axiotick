#pragma once

#include "core/types.h"
#include "core/errors.h"

#include <vector>

namespace argentum::codec {

ArgentumStatus encode_market_tick_legacy(const MarketTick& tick, std::vector<uint8_t>* out);
ArgentumStatus decode_market_tick(const void* data, size_t size, MarketTick* out);

#ifdef ARGENTUM_USE_FLATBUFFERS
ArgentumStatus encode_market_tick_flatbuffers(const MarketTick& tick, std::vector<uint8_t>* out, bool with_crc);
#endif

} // namespace argentum::codec
