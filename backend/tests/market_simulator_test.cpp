#include "core/fixed_point.hpp"
#include "gateway/market_simulator.hpp"
#include "gateway/smart_order_router.hpp"

#include <cstring>
#include <vector>

#define CHECK(cond) do { if (!(cond)) return 1; } while (0)

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
}

int main() {
    argentum::gateway::SmartOrderRouter router({
        argentum::gateway::VenueCostProfile{"V1", 0.5, 0.2, 0.5},
        argentum::gateway::VenueCostProfile{"V2", 0.2, 0.2, 1.0}
    });

    std::vector<argentum::gateway::VenueQuote> quotes;
    quotes.push_back(argentum::gateway::VenueQuote{
        "V1", "BTC/USDT",
        argentum::core::to_price_ticks(100.0), argentum::core::to_price_ticks(100.1),
        argentum::core::to_quantity_lots(2.0), argentum::core::to_quantity_lots(2.0),
        1
    });
    quotes.push_back(argentum::gateway::VenueQuote{
        "V2", "BTC/USDT",
        argentum::core::to_price_ticks(99.9), argentum::core::to_price_ticks(100.0),
        argentum::core::to_quantity_lots(2.0), argentum::core::to_quantity_lots(2.0),
        1
    });

    Order buy = make_order(7101, SIDE_BUY, TIF_GTC, 100.2, 3.0);
    auto route = router.route(buy, quotes);
    CHECK(route.accepted);

    argentum::gateway::MarketExecutionSimulator sim(12345);
    sim.upsert_venue_state(argentum::gateway::SimulationVenueState{
        "V1",
        argentum::core::to_quantity_lots(2.0),
        argentum::core::to_quantity_lots(0.5),
        0.95,
        4.0,
        argentum::gateway::LatencyProfile{0.2, 0.1}
    });
    sim.upsert_venue_state(argentum::gateway::SimulationVenueState{
        "V2",
        argentum::core::to_quantity_lots(2.0),
        argentum::core::to_quantity_lots(1.0),
        0.90,
        6.0,
        argentum::gateway::LatencyProfile{0.4, 0.2}
    });

    auto result = sim.simulate(buy, route, quotes);
    CHECK(result.requested_lots == route.requested_lots);
    CHECK(result.executed_lots <= route.requested_lots);
    CHECK(result.executed_lots > 0);
    CHECK(result.remaining_lots == (route.requested_lots - result.executed_lots));
    CHECK(result.avg_slippage_bps >= 0.0);
    CHECK(result.p95_latency_ms > 0.0);
    CHECK(result.fills.size() == route.legs.size());

    return 0;
}
