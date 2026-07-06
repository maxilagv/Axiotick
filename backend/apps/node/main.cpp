#include <iostream>
#include <memory>
#include <vector>
#include <thread>
#include <chrono>
#include <cstring>
#include <cstdlib>

#include "bus/message_bus.hpp"
#include "bus/message_protocol.hpp"
#include "datafeed/feed_player.hpp"
#include "persist/data_writer.hpp"
#include "persist/event_journal.hpp"
#include "trading/order_manager.hpp"
#include "risk/risk_manager.hpp"
#include "engine/order_book.hpp"
#include "benchmark/latency_tester.hpp"
#include "gateway/exchange_gateway.hpp"
#include "codec/market_tick_codec.hpp"
#include "alerts/alert_system.hpp"
#include "system/cpu_utils.hpp"
#include "audit/logger.hpp"
#include "api/market_gateway.hpp"
#include "api/http_ws_server.hpp"

int main() {
    // 1. System Init
    argentum::system::set_thread_name("main");
    argentum::system::pin_thread_to_core(0);
    argentum::audit::Logger::instance().log(argentum::audit::LogLevel::INFO, "System Booting...");

    // 2. Alert System
    argentum::alerts::AlertSystem alerts;
    alerts.register_handler(argentum::alerts::AlertSystem::console_handler);
    alerts.dispatch(argentum::alerts::AlertSeverity::INFO, "Alert System Online.");

    // 3. Exchange Gateway
    argentum::gateway::BinanceAdapter binance;
    binance.connect();
    binance.subscribe_market_data("BTCUSDT");

    // 4. Message Bus + Persistence
    argentum::bus::InprocBusConfig bus_config{};
    bus_config.queue_capacity = 65536;
    bus_config.policy = argentum::bus::BackpressurePolicy::DropNewest;
    bus_config.consumer_threads = 1;
    bus_config.worker_core_ids = {1};
    bus_config.worker_realtime_priority = 0;
    bus_config.disable_cpu_scaling = false;
    bus_config.busy_poll_during_market_hours = true;
    bus_config.worker_name_prefix = "market-data";
    auto bus = argentum::bus::create_inproc_bus(bus_config);
    bus->connect("inproc://market", true);

    argentum::persist::DataWriterService writer;
    writer.set_max_batch(256);
    writer.set_flush_interval_ms(50);
    writer.start();

    bus->subscribe("market.ticks", [&](const void* data, size_t size) {
        MarketTick tick{};
        if (argentum::codec::decode_market_tick(data, size, &tick) != ARGENTUM_OK) return;
        writer.enqueue(tick);
    });

    auto book = std::make_shared<argentum::engine::OrderBook>("BTC/USDT");
    auto risk = std::make_shared<argentum::risk::RiskManager>(argentum::risk::RiskLimits{
        5'000'000.0,
        20'000'000.0,
        1'000'000.0
    });
    auto event_journal = std::make_shared<argentum::persist::EventJournal>("data/order_events.jsonl");
    argentum::trading::OrderManager oms(risk, book, event_journal);

    argentum::api::GatewaySecurityConfig security{};
    if (const char* token_env = std::getenv("ARGENTUM_API_TOKEN")) {
        security.api_token = token_env;
    } else {
        security.api_token = "dev-token";
    }
    security.rate_limit.max_requests = 240;
    security.rate_limit.window_ms = 1000;

    argentum::api::MarketGatewayService gateway(bus, "market.ticks", security, event_journal);
    gateway.start();

    uint16_t api_port = 8080;
    if (const char* port_env = std::getenv("ARGENTUM_API_PORT")) {
        const long parsed = std::strtol(port_env, nullptr, 10);
        if (parsed > 0 && parsed <= 65535) {
            api_port = static_cast<uint16_t>(parsed);
        }
    }

    argentum::api::HttpWsServerConfig server_cfg{};
    server_cfg.max_requests_per_ip = 600;
    server_cfg.ip_window_ms = 1000;
    argentum::api::HttpWsServer api_server(bus, gateway, oms, api_port, "market.ticks", server_cfg);
    if (api_server.start()) {
        ARGENTUM_LOG(INFO, "[API] HTTP/WS gateway running on port " << api_port);
    } else {
        ARGENTUM_LOG(ERROR, "[API] Failed to start HTTP/WS gateway on port " << api_port);
    }

    argentum::datafeed::FeedPlayer player(bus, "market.ticks");
    size_t published = player.play_file("data/sample_ticks.jsonl", FEED_FORMAT_JSON, 0);
    if (published == 0) {
        MarketTick tick{};
        tick.timestamp_ns = 1700000000000000000ULL;
        tick.price = 50000.0;
        tick.quantity = 0.1;
        std::strncpy(tick.symbol, "BTC/USDT", sizeof(tick.symbol) - 1);
        std::strncpy(tick.source, "BINANCE", sizeof(tick.source) - 1);
        tick.side = SIDE_BUY;
        std::vector<uint8_t> payload;
        bool published_ok = false;
#ifdef ARGENTUM_USE_FLATBUFFERS
        if (argentum::codec::encode_market_tick_flatbuffers(tick, &payload, false) == ARGENTUM_OK) {
            if (bus->publish("market.ticks", payload.data(), payload.size()) == ARGENTUM_OK) {
                published_ok = true;
            }
        }
#else
        if (argentum::codec::encode_market_tick_legacy(tick, &payload) == ARGENTUM_OK) {
            if (bus->publish("market.ticks", payload.data(), payload.size()) == ARGENTUM_OK) {
                published_ok = true;
            }
        }
#endif
        if (published_ok) {
            published = 1;
        }
    }
        ARGENTUM_LOG(INFO, "[Datafeed] Published " << published << " ticks.");
    for (int i = 0; i < 20; ++i) {
        MarketTick latest{};
        if (gateway.get_latest_tick("BTC/USDT", &latest)) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ARGENTUM_LOG(INFO, "[API] Snapshot BTC/USDT: " << gateway.latest_tick_json("BTC/USDT"));
    ARGENTUM_LOG(INFO, "[API] Health: " << gateway.health_json());

    // 4. Latency Benchmark (Phase 16)
    argentum::benchmark::LatencyTester tester;
    tester.start(1000000); // 1 Million Ops
    tester.report();

    ARGENTUM_LOG(INFO, "[Argentum-FX] Main Loop Entering Wait State...");
    // In real app: while(running) { poll(); }

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    api_server.stop();
    writer.stop();
    gateway.stop();

    return 0;
}
