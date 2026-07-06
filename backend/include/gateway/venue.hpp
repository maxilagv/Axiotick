#pragma once

#include "core/types.h"

#include <cstdint>
#include <string>
#include <vector>

namespace argentum::gateway {

struct InstrumentMetadata {
    std::string canonical_symbol;
    int64_t tick_size_ticks = 1;
    int64_t lot_size_lots = 1;
};

struct VenueDescriptor {
    std::string venue_id;
    double taker_fee_bps = 0.0;
    double maker_fee_bps = 0.0;
    double expected_latency_ms = 1.0;
    bool supports_market = true;
    bool supports_limit = true;
};

struct VenueQuote {
    std::string venue_id;
    std::string symbol;
    int64_t bid_price_ticks = 0;
    int64_t ask_price_ticks = 0;
    int64_t bid_size_lots = 0;
    int64_t ask_size_lots = 0;
    uint64_t timestamp_ns = 0;
};

struct QuoteLevel {
    int64_t price_ticks = 0;
    int64_t size_lots = 0;
};

struct VenueOrderBookSnapshot {
    std::string venue_id;
    std::string symbol;
    std::vector<QuoteLevel> bid_levels;
    std::vector<QuoteLevel> ask_levels;
    uint64_t timestamp_ns = 0;
};

inline VenueOrderBookSnapshot snapshot_from_top_of_book(const VenueQuote& quote) {
    VenueOrderBookSnapshot snapshot{};
    snapshot.venue_id = quote.venue_id;
    snapshot.symbol = quote.symbol;
    snapshot.timestamp_ns = quote.timestamp_ns;
    if (quote.bid_price_ticks > 0 && quote.bid_size_lots > 0) {
        snapshot.bid_levels.push_back(QuoteLevel{quote.bid_price_ticks, quote.bid_size_lots});
    }
    if (quote.ask_price_ticks > 0 && quote.ask_size_lots > 0) {
        snapshot.ask_levels.push_back(QuoteLevel{quote.ask_price_ticks, quote.ask_size_lots});
    }
    return snapshot;
}

} // namespace argentum::gateway
