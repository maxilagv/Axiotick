#include "datafeed/market_parser.h"
#include "gateway/fix_adapter.hpp"

#include <cstring>
#include <unordered_map>

#define CHECK(cond) do { if (!(cond)) return 1; } while (0)

int main() {
    const std::string raw =
        "8=FIX.4.4|35=W|55=BTCUSDT|54=1|132=100.10|133=100.30|134=2.5|135=3.0|44=100.20|38=2.0|60=1700000000000000000|";

    argentum::gateway::FixAdapterV1 adapter("VENUE_FIX");
    argentum::gateway::VenueQuote quote{};
    CHECK(adapter.parse_market_data(raw, &quote));

    CHECK(quote.venue_id == "VENUE_FIX");
    CHECK(quote.symbol == "BTC/USDT");
    CHECK(quote.bid_price_ticks > 0);
    CHECK(quote.ask_price_ticks > quote.bid_price_ticks);
    CHECK(quote.bid_size_lots > 0);
    CHECK(quote.ask_size_lots > 0);
    CHECK(quote.timestamp_ns == 1700000000000000000ULL);

    std::unordered_map<int, std::string> fields;
    CHECK(argentum::gateway::FixAdapterV1::parse_fields(raw, &fields, '|'));
    CHECK(fields[55] == "BTCUSDT");
    CHECK(fields[132] == "100.10");

    CHECK(argentum::gateway::FixAdapterV1::normalize_symbol("ethusdt") == "ETH/USDT");
    CHECK(argentum::gateway::FixAdapterV1::normalize_symbol("eurusd") == "EUR/USD");

    MarketTick tick{};
    const ArgentumStatus status = parse_market_message(
        FEED_FORMAT_FIX,
        raw.c_str(),
        raw.size(),
        &tick);
    CHECK(status == ARGENTUM_OK);
    CHECK(std::strcmp(tick.symbol, "BTC/USDT") == 0);
    CHECK(tick.side == SIDE_BUY || tick.side == SIDE_SELL);

    return 0;
}
