#include "api/market_gateway.hpp"
#include "bus/message_bus.hpp"
#include "codec/market_tick_codec.hpp"

#include <cassert>
#include <chrono>
#include <cstring>
#include <thread>

int main() {
    argentum::bus::InprocBusConfig config;
    config.consumer_threads = 1;
    config.queue_capacity = 64;

    auto bus = argentum::bus::create_inproc_bus(config);
    bus->connect("inproc://contract", true);

    argentum::api::MarketGatewayService gateway(bus, "market.ticks");
    gateway.start();

    MarketTick tick{};
    tick.timestamp_ns = 1700000001111111111ULL;
    tick.price = 50123.45;
    tick.quantity = 0.25;
    tick.side = SIDE_BUY;
    std::strncpy(tick.symbol, "BTC/USDT", sizeof(tick.symbol) - 1);
    std::strncpy(tick.source, "BINANCE", sizeof(tick.source) - 1);

    std::vector<uint8_t> payload;
    assert(argentum::codec::encode_market_tick_legacy(tick, &payload) == ARGENTUM_OK);
    assert(bus->publish("market.ticks", payload.data(), payload.size()) == ARGENTUM_OK);

    MarketTick latest{};
    bool found = false;
    for (int i = 0; i < 100; ++i) {
        found = gateway.get_latest_tick("btcusdt", &latest);
        if (found) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(found);
    assert(latest.timestamp_ns == tick.timestamp_ns);
    assert(latest.price == tick.price);
    assert(latest.quantity == tick.quantity);

    std::string tick_json = gateway.latest_tick_json("BTCUSDT");
    assert(tick_json.find("\"event\":\"tick\"") != std::string::npos);
    assert(tick_json.find("\"symbol\":\"BTC/USDT\"") != std::string::npos);
    assert(tick_json.find("\"source\":\"BINANCE\"") != std::string::npos);

    auto metrics = gateway.metrics();
    assert(metrics.ticks_received >= 1);
    assert(metrics.ticks_decoded >= 1);
    assert(metrics.tracked_symbols >= 1);

    std::string health = gateway.health_json();
    assert(health.find("\"status\":\"ok\"") != std::string::npos);
    assert(health.find("\"tracked_symbols\":") != std::string::npos);

    gateway.stop();
    std::string health_after_stop = gateway.health_json();
    assert(health_after_stop.find("\"status\":\"down\"") != std::string::npos);
    return 0;
}
