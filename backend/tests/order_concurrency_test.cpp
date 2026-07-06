#include "engine/order_book.hpp"
#include "risk/risk_manager.hpp"
#include "trading/order_manager.hpp"

#include <atomic>
#include <cassert>
#include <cmath>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

namespace {
bool almost_equal(double a, double b, double eps = 1e-9) {
    return std::abs(a - b) <= eps;
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

    constexpr int kThreads = 4;
    constexpr int kPerThread = 200;
    std::atomic<int> submit_failures{0};
    std::atomic<int> cancel_failures{0};

    std::vector<std::thread> workers;
    workers.reserve(kThreads);

    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([t, &oms, &submit_failures, &cancel_failures] {
            for (int i = 0; i < kPerThread; ++i) {
                Order order{};
                order.order_id = static_cast<uint64_t>(t) * 100000ULL + static_cast<uint64_t>(i) + 1ULL;
                order.timestamp_ns = static_cast<uint64_t>(i + 1);
                order.side = static_cast<uint8_t>(((i % 2) == 0) ? SIDE_BUY : SIDE_SELL);
                order.type = ORDER_TYPE_LIMIT;
                order.tif = TIF_GTC;
                order.price = (order.side == SIDE_BUY) ? 90.0 : 110.0; // keep non-crossing
                order.quantity = 1.0;
                std::strncpy(order.symbol, "BTC/USDT", sizeof(order.symbol) - 1);

                auto result = oms.submit_order(order);
                if (!result.accepted || !result.resting) {
                    submit_failures.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                if (!oms.cancel_order(order.order_id)) {
                    cancel_failures.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    for (auto& worker : workers) {
        worker.join();
    }

    assert(submit_failures.load(std::memory_order_relaxed) == 0);
    assert(cancel_failures.load(std::memory_order_relaxed) == 0);
    assert(oms.active_order_count() == 0);
    assert(almost_equal(risk->committed_exposure(), 0.0));
    return 0;
}
