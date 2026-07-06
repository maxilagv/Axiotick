#include "api/market_gateway.hpp"
#include "core/id_generator.hpp"
#include "engine/order_book.hpp"
#include "risk/risk_manager.hpp"

#include <cassert>
#include <cstring>
#include <memory>

int main() {
    const uint64_t first = argentum::core::next_order_id();
    const uint64_t second = argentum::core::next_order_id();
    if (second != first + 1) {
        return 1;
    }

    auto book = std::make_shared<argentum::engine::OrderBook>("BTC/USDT");
    auto risk = std::make_shared<argentum::risk::RiskManager>(argentum::risk::RiskLimits{
        10'000'000.0,
        10'000'000.0,
        10'000'000.0
    });
    argentum::trading::OrderManager oms(risk, book);

    Order order{};
    order.order_id = 0;
    order.side = SIDE_BUY;
    order.type = ORDER_TYPE_LIMIT;
    order.tif = TIF_GTC;
    order.price = 90.0;
    order.quantity = 1.0;
    std::strncpy(order.symbol, "BTC/USDT", sizeof(order.symbol) - 1);

    const argentum::api::OrderAck ack = argentum::api::submit_order(oms, order);
    assert(ack.order_id != 0);
    assert(ack.accepted);
    assert(ack.resting);

    return 0;
}
