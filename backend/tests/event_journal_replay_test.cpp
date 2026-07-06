#include "api/market_gateway.hpp"
#include "bus/message_bus.hpp"
#include "engine/order_book.hpp"
#include "persist/event_journal.hpp"
#include "risk/risk_manager.hpp"
#include "trading/order_manager.hpp"

#include <chrono>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>

#define CHECK(cond) do { if (!(cond)) { std::cerr << "CHECK failed: " << #cond << " line=" << __LINE__ << std::endl; return 1; } } while (0)

namespace {
Order make_order(
    uint64_t id,
    Side side,
    OrderType type,
    TimeInForce tif,
    double px,
    double qty,
    uint64_t ts) {
    Order o{};
    o.order_id = id;
    o.side = static_cast<uint8_t>(side);
    o.type = static_cast<uint8_t>(type);
    o.tif = static_cast<uint8_t>(tif);
    o.price = px;
    o.quantity = qty;
    o.timestamp_ns = ts;
    std::strncpy(o.symbol, "BTC/USDT", sizeof(o.symbol) - 1);
    return o;
}
}

int main() {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::string journal_path = "data/test_order_events_" + std::to_string(nonce) + ".jsonl";
    std::error_code ec;
    std::filesystem::remove(journal_path, ec);

    {
        auto journal = std::make_shared<argentum::persist::EventJournal>(journal_path);

        auto bus = argentum::bus::create_inproc_bus();
        argentum::api::GatewaySecurityConfig security{};
        security.api_token = "phase2-token";
        security.rate_limit.max_requests = 100;
        security.rate_limit.window_ms = 1000;
        argentum::api::MarketGatewayService gateway(bus, "market.ticks", security, journal);

        auto book = std::make_shared<argentum::engine::OrderBook>("BTC/USDT");
        auto risk = std::make_shared<argentum::risk::RiskManager>(argentum::risk::RiskLimits{
            10'000'000.0,
            10'000'000.0,
            10'000'000.0
        });
        argentum::trading::OrderManager oms(risk, book, journal);

        // Unauthorized request should be journaled as gateway_rejected.
        auto bad = make_order(5000, SIDE_BUY, ORDER_TYPE_LIMIT, TIF_GTC, 100.0, 0.5, 1);
        auto bad_ack = argentum::api::submit_order(gateway, oms, bad, "invalid-token");
        CHECK(!bad_ack.accepted);
        CHECK(bad_ack.gateway_reject_reason == argentum::api::GatewayRejectReason::Unauthorized);

        // Resting maker and IOC taker to generate accepted + trades.
        auto maker = make_order(1001, SIDE_SELL, ORDER_TYPE_LIMIT, TIF_GTC, 100.0, 1.0, 2);
        auto maker_result = oms.submit_order(maker);
        CHECK(maker_result.accepted);
        CHECK(maker_result.resting);

        auto taker = make_order(1002, SIDE_BUY, ORDER_TYPE_LIMIT, TIF_IOC, 100.0, 2.0, 3);
        auto taker_result = oms.submit_order(taker);
        CHECK(taker_result.accepted);
        CHECK(!taker_result.resting);
        CHECK(taker_result.trades.size() == 1);

        // Invalid order reject (keeps maker lifecycle intact by using a different order_id).
        auto invalid = make_order(3003, SIDE_BUY, ORDER_TYPE_MARKET, TIF_IOC, 0.0, 0.2, 4);
        auto invalid_result = oms.submit_order(invalid);
        CHECK(!invalid_result.accepted);

        journal->flush();
    }

    argentum::persist::ReplaySummary summary{};
    CHECK(argentum::persist::EventReplayer::replay_file(journal_path, &summary));

    CHECK(summary.total_events >= 5);
    CHECK(summary.accepted >= 2);
    CHECK(summary.rejected >= 1);
    CHECK(summary.gateway_rejected == 1);
    CHECK(summary.trades == 1);
    CHECK(summary.monotonic_seq);
    CHECK(summary.monotonic_time);
    CHECK(summary.active_orders.empty());
    CHECK(summary.committed_exposure_units == 0);
    CHECK(summary.net_position_lots == 0);

    auto maker_hist_it = summary.order_history.find(1001);
    CHECK(maker_hist_it != summary.order_history.end());
    CHECK(maker_hist_it->second.remaining_lots == 0);

    auto taker_hist_it = summary.order_history.find(1002);
    CHECK(taker_hist_it != summary.order_history.end());
    CHECK(taker_hist_it->second.filled_lots > 0);

    std::filesystem::remove(journal_path, ec);
    return 0;
}
