#include "core/fixed_point.hpp"
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
        argentum::gateway::VenueCostProfile{"VFAST", 2.0, 0.2, 0.2},
        argentum::gateway::VenueCostProfile{"VSLOW", 0.0, 1.0, 20.0}
    });

    std::vector<argentum::gateway::VenueQuote> quotes;
    quotes.push_back(argentum::gateway::VenueQuote{
        "VFAST", "BTC/USDT",
        argentum::core::to_price_ticks(99.9), argentum::core::to_price_ticks(100.0),
        argentum::core::to_quantity_lots(2.0), argentum::core::to_quantity_lots(2.0),
        1
    });
    quotes.push_back(argentum::gateway::VenueQuote{
        "VSLOW", "BTC/USDT",
        argentum::core::to_price_ticks(99.8), argentum::core::to_price_ticks(99.95),
        argentum::core::to_quantity_lots(5.0), argentum::core::to_quantity_lots(5.0),
        1
    });

    Order buy = make_order(7001, SIDE_BUY, TIF_GTC, 100.1, 3.0);
    auto decision = router.route(buy, quotes);

    CHECK(decision.accepted);
    CHECK(decision.routed_lots == argentum::core::to_quantity_lots(3.0));
    CHECK(decision.legs.size() == 2);
    CHECK(decision.legs[0].venue_id == "VFAST");
    CHECK(decision.legs[0].requested_lots == argentum::core::to_quantity_lots(2.0));

    Order fok = make_order(7002, SIDE_BUY, TIF_FOK, 100.1, 10.0);
    auto fok_decision = router.route(fok, quotes);
    CHECK(!fok_decision.accepted);
    CHECK(fok_decision.reject_reason == "insufficient_liquidity_fok");

    return 0;
}
