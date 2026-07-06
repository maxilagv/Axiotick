#include "core/fixed_point.hpp"
#include "core/time_utils.hpp"
#include "engine/order_book.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <vector>

namespace {

Order make_limit_order(uint64_t order_id, Side side, double price, double quantity, uint64_t timestamp_ns) {
    Order order{};
    order.order_id = order_id;
    order.side = static_cast<uint8_t>(side);
    order.type = ORDER_TYPE_LIMIT;
    order.tif = TIF_GTC;
    order.price = price;
    order.quantity = quantity;
    order.timestamp_ns = timestamp_ns;
    std::strncpy(order.symbol, "EUR/USD", sizeof(order.symbol) - 1);
    argentum::core::normalize_order_scalars(&order);
    return order;
}

size_t percentile_index(size_t size, double q) {
    size_t index = static_cast<size_t>(static_cast<double>(size) * q);
    if (index >= size) {
        index = size - 1;
    }
    return index;
}

} // namespace

int main() {
    constexpr size_t kIterations = 1000000;
    constexpr size_t kWarmBookDepth = 1024;
    constexpr double kPrice = 100.0;
    constexpr double kQuantity = 1.0;

    auto book = std::make_unique<argentum::engine::OrderBook>("EUR/USD");
    for (size_t i = 0; i < kWarmBookDepth; ++i) {
        const auto maker = make_limit_order(1000 + static_cast<uint64_t>(i), SIDE_SELL, kPrice, kQuantity, i + 1);
        assert(book->add_order(maker));
    }

    std::vector<uint64_t> latencies_ns;
    latencies_ns.reserve(kIterations);

    uint64_t next_maker_id = 1000 + static_cast<uint64_t>(kWarmBookDepth);
    uint64_t next_taker_id = 100000000;

    for (size_t i = 0; i < kIterations; ++i) {
        Order taker = make_limit_order(next_taker_id++, SIDE_BUY, kPrice, kQuantity, argentum::core::now_ns());

        const auto start = std::chrono::high_resolution_clock::now();
        const auto trades = book->match_order(taker, false);
        const auto end = std::chrono::high_resolution_clock::now();
        const uint64_t elapsed = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
        latencies_ns.push_back(elapsed);

        assert(trades.size() == 1);
        assert(trades[0].quantity_lots == taker.quantity_lots);

        const auto maker = make_limit_order(next_maker_id++, SIDE_SELL, kPrice, kQuantity, argentum::core::now_ns());
        assert(book->add_order(maker));
    }

    std::sort(latencies_ns.begin(), latencies_ns.end());
    const double p50_ns = static_cast<double>(latencies_ns[percentile_index(latencies_ns.size(), 0.50)]);
    const double p95_ns = static_cast<double>(latencies_ns[percentile_index(latencies_ns.size(), 0.95)]);
    const double p99_ns = static_cast<double>(latencies_ns[percentile_index(latencies_ns.size(), 0.99)]);
    const double p999_ns = static_cast<double>(latencies_ns[percentile_index(latencies_ns.size(), 0.999)]);

    std::cout << "[Matching Benchmark] Iterations: " << kIterations << "\n";
    std::cout << "[Matching Benchmark] P50: " << p50_ns << " ns\n";
    std::cout << "[Matching Benchmark] P95: " << p95_ns << " ns\n";
    std::cout << "[Matching Benchmark] P99: " << p99_ns << " ns\n";
    std::cout << "[Matching Benchmark] P99.9: " << p999_ns << " ns\n";

    return 0;
}
