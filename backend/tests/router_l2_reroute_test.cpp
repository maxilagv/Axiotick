#include "core/fixed_point.hpp"
#include "gateway/market_simulator.hpp"
#include "gateway/smart_order_router.hpp"

#include <cstring>
#include <initializer_list>
#include <iostream>
#include <vector>

#define CHECK(cond) do { if (!(cond)) { std::cerr << "CHECK failed: " << #cond << " line=" << __LINE__ << std::endl; return 1; } } while (0)

namespace {
Order make_order(uint64_t id, Side side, TimeInForce tif, double price, double qty) {
    Order o{};
    o.order_id = id;
    o.side = static_cast<uint8_t>(side);
    o.type = ORDER_TYPE_LIMIT;
    o.tif = static_cast<uint8_t>(tif);
    o.price = price;
    o.quantity = qty;
    std::strncpy(o.symbol, "BTC/USDT", sizeof(o.symbol) - 1);
    return o;
}

argentum::gateway::VenueOrderBookSnapshot make_book(
    const char* venue,
    std::initializer_list<std::pair<double, double>> asks,
    std::initializer_list<std::pair<double, double>> bids = {}) {
    argentum::gateway::VenueOrderBookSnapshot book{};
    book.venue_id = venue;
    book.symbol = "BTC/USDT";

    for (const auto& [px, qty] : bids) {
        book.bid_levels.push_back(argentum::gateway::QuoteLevel{
            argentum::core::to_price_ticks(px),
            argentum::core::to_quantity_lots(qty)
        });
    }
    for (const auto& [px, qty] : asks) {
        book.ask_levels.push_back(argentum::gateway::QuoteLevel{
            argentum::core::to_price_ticks(px),
            argentum::core::to_quantity_lots(qty)
        });
    }
    return book;
}
}

int main() {
    argentum::gateway::SmartOrderRouter router({
        argentum::gateway::VenueCostProfile{"V1", 0.5, 0.2, 0.4},
        argentum::gateway::VenueCostProfile{"V2", 0.2, 0.2, 0.7}
    });

    std::vector<argentum::gateway::VenueOrderBookSnapshot> books;
    books.push_back(make_book("V1", {{100.00, 2.0}, {100.05, 2.0}}));
    books.push_back(make_book("V2", {{99.95, 2.0}, {100.10, 2.0}}));

    Order buy = make_order(8101, SIDE_BUY, TIF_GTC, 100.10, 4.0);
    auto l2 = router.route_l2(buy, books);
    CHECK(l2.accepted);
    CHECK(l2.routed_lots == argentum::core::to_quantity_lots(4.0));
    CHECK(l2.legs.size() >= 2);

    argentum::gateway::MarketExecutionSimulator sim(42);
    sim.upsert_venue_state(argentum::gateway::SimulationVenueState{
        "V1",
        argentum::core::to_quantity_lots(2.0),
        argentum::core::to_quantity_lots(1.0),
        1.0,
        3.0,
        argentum::gateway::LatencyProfile{0.2, 0.0}
    });
    sim.upsert_venue_state(argentum::gateway::SimulationVenueState{
        "V2",
        argentum::core::to_quantity_lots(2.0),
        argentum::core::to_quantity_lots(1.0),
        1.0,
        2.0,
        argentum::gateway::LatencyProfile{0.3, 0.0}
    });

    auto result = sim.simulate_with_rerouting(buy, router, books, 3);
    CHECK(result.requested_lots == argentum::core::to_quantity_lots(4.0));
    CHECK(result.executed_lots >= argentum::core::to_quantity_lots(3.0));
    CHECK(result.remaining_lots <= argentum::core::to_quantity_lots(1.0));
    CHECK(result.fills.size() >= 3); // multiple passes / legs
    CHECK(result.p95_latency_ms >= 0.2);

    auto reroute = router.reroute_after_partial_fill(
        buy,
        argentum::core::to_quantity_lots(2.0),
        books);
    CHECK(reroute.accepted);
    CHECK(reroute.requested_lots == argentum::core::to_quantity_lots(2.0));

    return 0;
}
