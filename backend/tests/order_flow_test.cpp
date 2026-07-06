#include "engine/order_book.hpp"
#include "risk/risk_manager.hpp"
#include "trading/order_manager.hpp"

#include <cassert>
#include <cmath>
#include <memory>
#include <cstring>

namespace {
bool almost_equal(double a, double b, double eps = 1e-9) {
    return std::abs(a - b) <= eps;
}
}

int main() {
    auto book = std::make_shared<argentum::engine::OrderBook>("BTC/USDT");
    argentum::risk::RiskLimits limits{
        1'000'000.0,
        1'000'000.0,
        1'000'000.0
    };
    auto risk = std::make_shared<argentum::risk::RiskManager>(limits);
    argentum::trading::OrderManager oms(risk, book);

    Order maker{};
    maker.order_id = 1001;
    maker.price = 100.0;
    maker.quantity = 1.0;
    maker.side = SIDE_SELL;
    maker.type = ORDER_TYPE_LIMIT;
    maker.timestamp_ns = 1;
    std::strncpy(maker.symbol, "BTC/USDT", sizeof(maker.symbol) - 1);

    auto maker_result = oms.submit_order(maker);
    assert(maker_result.accepted);
    assert(maker_result.resting);
    assert(almost_equal(maker_result.remaining_quantity, 1.0));
    assert(oms.active_order_count() == 1);

    auto dup_result = oms.submit_order(maker);
    assert(!dup_result.accepted);
    assert(dup_result.reject_reason == argentum::trading::OrderRejectReason::DuplicateOrderId);

    Order taker{};
    taker.order_id = 2002;
    taker.price = 100.0;
    taker.quantity = 0.4;
    taker.side = SIDE_BUY;
    taker.type = ORDER_TYPE_MARKET;
    taker.timestamp_ns = 2;
    std::strncpy(taker.symbol, "BTC/USDT", sizeof(taker.symbol) - 1);

    auto taker_result = oms.submit_order(taker);
    assert(taker_result.accepted);
    assert(!taker_result.resting);
    assert(taker_result.trades.size() == 1);
    assert(almost_equal(taker_result.filled_quantity, 0.4));
    assert(almost_equal(taker_result.remaining_quantity, 0.0));

    assert(oms.active_order_count() == 1);

    Order invalid_market{};
    invalid_market.order_id = 3003;
    invalid_market.price = 0.0;
    invalid_market.quantity = 0.2;
    invalid_market.side = SIDE_BUY;
    invalid_market.type = ORDER_TYPE_MARKET;
    invalid_market.timestamp_ns = 3;
    std::strncpy(invalid_market.symbol, "BTC/USDT", sizeof(invalid_market.symbol) - 1);
    auto invalid_market_result = oms.submit_order(invalid_market);
    assert(!invalid_market_result.accepted);
    assert(invalid_market_result.reject_reason == argentum::trading::OrderRejectReason::InvalidOrder);

    auto ask = book->get_best_ask();
    assert(ask.has_value());
    assert(almost_equal(*ask, 100.0));

    assert(oms.cancel_order(1001));
    assert(oms.active_order_count() == 0);
    assert(!oms.cancel_order(1001));

    assert(almost_equal(risk->committed_exposure(), 0.0));
    return 0;
}
