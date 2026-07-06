#include "engine/order_book.hpp"
#include "risk/risk_manager.hpp"
#include "trading/order_manager.hpp"

#include <cassert>
#include <cstring>
#include <memory>

namespace {

Order make_order(uint64_t order_id, Side side) {
    Order order{};
    order.order_id = order_id;
    order.timestamp_ns = order_id;
    order.side = static_cast<uint8_t>(side);
    order.type = ORDER_TYPE_LIMIT;
    order.tif = TIF_GTC;
    order.price = (side == SIDE_BUY) ? 90.0 : 110.0;
    order.quantity = 1.0;
    std::strncpy(order.symbol, "BTC/USDT", sizeof(order.symbol) - 1);
    return order;
}

}

int main() {
    auto book = std::make_shared<argentum::engine::OrderBook>("BTC/USDT");
    auto risk = std::make_shared<argentum::risk::RiskManager>(argentum::risk::RiskLimits{
        10'000'000.0,
        10'000'000.0,
        10'000'000.0
    });
    argentum::trading::OrderManager oms(risk, book);
    oms.set_history_cap(2);

    for (uint64_t order_id = 1; order_id <= 3; ++order_id) {
        auto result = oms.submit_order(make_order(order_id, (order_id % 2 == 0) ? SIDE_SELL : SIDE_BUY));
        assert(result.accepted);
        assert(result.resting);
        assert(oms.cancel_order(order_id));
    }

    assert(oms.history_order_count() == 2);

    argentum::trading::OrderState state{};
    assert(!oms.get_order_state(1, &state));
    assert(oms.get_order_state(2, &state));
    assert(oms.get_order_state(3, &state));

    return 0;
}
