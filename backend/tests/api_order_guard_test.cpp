#include "api/market_gateway.hpp"
#include "bus/message_bus.hpp"
#include "engine/order_book.hpp"
#include "risk/risk_manager.hpp"
#include "trading/order_manager.hpp"

#include <cassert>
#include <cstring>
#include <memory>

int main() {
    auto bus = argentum::bus::create_inproc_bus();
    argentum::api::GatewaySecurityConfig security;
    security.api_token = "secret-token";
    security.rate_limit.max_requests = 2;
    security.rate_limit.window_ms = 1000;
    argentum::api::MarketGatewayService gateway(bus, "market.ticks", security);
    gateway.start();

    auto book = std::make_shared<argentum::engine::OrderBook>("BTC/USDT");
    auto risk = std::make_shared<argentum::risk::RiskManager>(argentum::risk::RiskLimits{
        1'000'000.0,
        1'000'000.0,
        1'000'000.0
    });
    argentum::trading::OrderManager oms(risk, book);

    Order order1{};
    order1.order_id = 9001;
    order1.price = 100.0;
    order1.quantity = 1.0;
    order1.side = SIDE_BUY;
    order1.type = ORDER_TYPE_LIMIT;
    std::strncpy(order1.symbol, "BTC/USDT", sizeof(order1.symbol) - 1);

    Order order2 = order1;
    order2.order_id = 9002;
    order2.side = SIDE_SELL;

    Order order3 = order1;
    order3.order_id = 9003;

    auto unauthorized = argentum::api::submit_order(gateway, oms, order1, "invalid");
    assert(!unauthorized.accepted);
    assert(unauthorized.gateway_reject_reason == argentum::api::GatewayRejectReason::Unauthorized);

    auto ok1 = argentum::api::submit_order(gateway, oms, order1, "secret-token");
    assert(ok1.accepted);
    assert(ok1.gateway_reject_reason == argentum::api::GatewayRejectReason::None);

    auto ok2 = argentum::api::submit_order(gateway, oms, order2, "secret-token");
    assert(ok2.accepted);
    assert(ok2.gateway_reject_reason == argentum::api::GatewayRejectReason::None);

    auto limited = argentum::api::submit_order(gateway, oms, order3, "secret-token");
    assert(!limited.accepted);
    assert(limited.gateway_reject_reason == argentum::api::GatewayRejectReason::RateLimited);

    auto metrics = gateway.metrics();
    assert(metrics.order_requests == 4);
    assert(metrics.auth_failures == 1);
    assert(metrics.rate_limited == 1);
    assert(metrics.order_accepted == 2);
    assert(metrics.order_rejected >= 2);

    std::string ack_json = argentum::api::to_json(limited);
    assert(ack_json.find("\"gateway_reject_reason\":\"rate_limited\"") != std::string::npos);
    gateway.stop();
    return 0;
}
