// OrderBook::match_order latency, through the shared benchmark harness
// (warmup, unified clock, histogram percentiles, JSON output with
// machine-captured environment metadata). JSON path: argv[1], default
// benchmarks_out/matching.json.

#include "benchmark/harness.hpp"
#include "core/fixed_point.hpp"
#include "core/time_utils.hpp"
#include "engine/order_book.hpp"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <memory>

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

} // namespace

int main(int argc, char** argv) {
    constexpr uint64_t kWarmup = 10'000;
    constexpr uint64_t kIterations = 1'000'000;
    constexpr size_t kWarmBookDepth = 1024;
    constexpr double kPrice = 100.0;
    constexpr double kQuantity = 1.0;

    auto book = std::make_unique<argentum::engine::OrderBook>("EUR/USD");
    for (size_t i = 0; i < kWarmBookDepth; ++i) {
        const auto maker = make_limit_order(1000 + static_cast<uint64_t>(i), SIDE_SELL, kPrice, kQuantity, i + 1);
        assert(book->add_order(maker));
        (void)maker;
    }

    uint64_t next_maker_id = 1000 + static_cast<uint64_t>(kWarmBookDepth);
    uint64_t next_taker_id = 100'000'000;

    const argentum::core::LatencyReport report = argentum::benchmark::measure_loop(
        kWarmup, kIterations, [&](uint64_t) {
            const Order taker = make_limit_order(
                next_taker_id++, SIDE_BUY, kPrice, kQuantity, argentum::core::mono_now_ns());
            const auto trades = book->match_order(taker, false);
            assert(trades.size() == 1);
            assert(trades[0].quantity_lots == taker.quantity_lots);
            (void)trades;

            // Refill so the book depth stays constant across iterations.
            const auto maker = make_limit_order(
                next_maker_id++, SIDE_SELL, kPrice, kQuantity, argentum::core::mono_now_ns());
            assert(book->add_order(maker));
            (void)maker;
        });

    argentum::benchmark::BenchmarkReporter reporter("matching_benchmark");
    argentum::benchmark::BenchmarkCase result;
    result.name = "order_book_match_and_refill";
    result.warmup_iterations = kWarmup;
    result.iterations = kIterations;
    result.latency = report;
    result.extra.emplace_back("warm_book_depth", "1024");
    result.extra.emplace_back(
        "note", "each iteration = one full match + one refill add_order (book depth constant)");
    reporter.add(result);

    reporter.print_human(stdout);
    const char* json_path = (argc > 1) ? argv[1] : "benchmarks_out/matching.json";
    if (!reporter.write_json(json_path)) {
        std::fprintf(stderr, "[matching_benchmark] failed to write %s\n", json_path);
        return 1;
    }
    std::printf("[matching_benchmark] json: %s\n", json_path);
    return 0;
}
