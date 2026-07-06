#include "engine/order_book.hpp"
#include "risk/risk_manager.hpp"
#include "trading/order_manager.hpp"

#include <cassert>
#include <cmath>
#include <cstring>
#include <memory>

namespace {
bool almost_equal(double a, double b, double eps = 1e-9) {
    return std::abs(a - b) <= eps;
}

Order make_order(
    uint64_t order_id,
    Side side,
    OrderType type,
    TimeInForce tif,
    double price,
    double quantity,
    uint64_t ts) {
    Order order{};
    order.order_id = order_id;
    order.side = static_cast<uint8_t>(side);
    order.type = static_cast<uint8_t>(type);
    order.tif = static_cast<uint8_t>(tif);
    order.price = price;
    order.quantity = quantity;
    order.timestamp_ns = ts;
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

    // GTC: residual should rest.
    auto maker_gtc = make_order(1001, SIDE_SELL, ORDER_TYPE_LIMIT, TIF_GTC, 100.0, 1.0, 1);
    auto maker_gtc_result = oms.submit_order(maker_gtc);
    assert(maker_gtc_result.accepted);
    assert(maker_gtc_result.resting);

    auto taker_gtc = make_order(1002, SIDE_BUY, ORDER_TYPE_LIMIT, TIF_GTC, 100.0, 2.0, 2);
    auto taker_gtc_result = oms.submit_order(taker_gtc);
    assert(taker_gtc_result.accepted);
    assert(taker_gtc_result.resting);
    assert(almost_equal(taker_gtc_result.filled_quantity, 1.0));
    assert(almost_equal(taker_gtc_result.remaining_quantity, 1.0));
    assert(taker_gtc_result.status == argentum::trading::OrderStatus::PartiallyFilled);
    assert(oms.active_order_count() == 1);
    assert(oms.cancel_order(1002));

    // IOC: residual should not rest.
    auto maker_ioc = make_order(2001, SIDE_SELL, ORDER_TYPE_LIMIT, TIF_GTC, 100.0, 1.0, 3);
    auto maker_ioc_result = oms.submit_order(maker_ioc);
    assert(maker_ioc_result.accepted);
    assert(maker_ioc_result.resting);

    auto taker_ioc = make_order(2002, SIDE_BUY, ORDER_TYPE_LIMIT, TIF_IOC, 100.0, 2.0, 4);
    auto taker_ioc_result = oms.submit_order(taker_ioc);
    assert(taker_ioc_result.accepted);
    assert(!taker_ioc_result.resting);
    assert(almost_equal(taker_ioc_result.filled_quantity, 1.0));
    assert(almost_equal(taker_ioc_result.remaining_quantity, 1.0));
    assert(taker_ioc_result.status == argentum::trading::OrderStatus::PartiallyFilled);
    assert(oms.active_order_count() == 0);

    // FOK insufficient liquidity: rejected before matching.
    auto maker_fok_short = make_order(3001, SIDE_SELL, ORDER_TYPE_LIMIT, TIF_GTC, 100.0, 1.0, 5);
    auto maker_fok_short_result = oms.submit_order(maker_fok_short);
    assert(maker_fok_short_result.accepted);
    assert(maker_fok_short_result.resting);

    auto taker_fok_short = make_order(3002, SIDE_BUY, ORDER_TYPE_LIMIT, TIF_FOK, 100.0, 2.0, 6);
    auto taker_fok_short_result = oms.submit_order(taker_fok_short);
    assert(!taker_fok_short_result.accepted);
    assert(taker_fok_short_result.reject_reason == argentum::trading::OrderRejectReason::LiquidityUnavailable);
    assert(taker_fok_short_result.status == argentum::trading::OrderStatus::Rejected);
    assert(almost_equal(taker_fok_short_result.filled_quantity, 0.0));
    assert(almost_equal(taker_fok_short_result.remaining_quantity, 2.0));
    assert(oms.active_order_count() == 1);
    assert(oms.cancel_order(3001));

    // FOK with enough liquidity: full fill, no rest.
    auto maker_fok_full = make_order(4001, SIDE_SELL, ORDER_TYPE_LIMIT, TIF_GTC, 100.0, 2.0, 7);
    auto maker_fok_full_result = oms.submit_order(maker_fok_full);
    assert(maker_fok_full_result.accepted);
    assert(maker_fok_full_result.resting);

    auto taker_fok_full = make_order(4002, SIDE_BUY, ORDER_TYPE_LIMIT, TIF_FOK, 100.0, 2.0, 8);
    auto taker_fok_full_result = oms.submit_order(taker_fok_full);
    assert(taker_fok_full_result.accepted);
    assert(!taker_fok_full_result.resting);
    assert(taker_fok_full_result.status == argentum::trading::OrderStatus::Filled);
    assert(almost_equal(taker_fok_full_result.filled_quantity, 2.0));
    assert(almost_equal(taker_fok_full_result.remaining_quantity, 0.0));
    assert(oms.active_order_count() == 0);

    return 0;
}
