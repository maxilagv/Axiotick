// RiskManager::check_order latency under a realistic order mix — the risk
// check promised by Fase 01 that had never been measured. The mix alternates
// symbols, sides and notionals, includes a slice of orders that exceed the
// per-order value limit (rejected fast path) and periodic cancels so the
// reservation ledger keeps realistic occupancy instead of growing without
// bound. JSON path: argv[1], default benchmarks_out/risk.json.

#include "benchmark/harness.hpp"
#include "core/fixed_point.hpp"
#include "core/time_utils.hpp"
#include "risk/risk_manager.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>

namespace {

constexpr const char* kSymbols[4] = {"EUR/USD", "BTC/USDT", "ETH/USDT", "GBP/USD"};

Order make_order(uint64_t order_id, size_t variant) {
    Order order{};
    order.order_id = order_id;
    order.side = static_cast<uint8_t>((variant % 2 == 0) ? SIDE_BUY : SIDE_SELL);
    order.type = ORDER_TYPE_LIMIT;
    order.tif = TIF_GTC;
    order.price = 100.0 + static_cast<double>(variant % 100) * 0.25;
    // Every 16th order breaches max_order_value on purpose: the rejected fast
    // path belongs in the latency distribution too.
    order.quantity = (variant % 16 == 15) ? 10'000.0 : (1.0 + static_cast<double>(variant % 8));
    order.timestamp_ns = argentum::core::wall_now_ns();
    std::strncpy(order.symbol, kSymbols[variant % 4], sizeof(order.symbol) - 1);
    argentum::core::normalize_order_scalars(&order);
    return order;
}

} // namespace

int main(int argc, char** argv) {
    constexpr uint64_t kWarmup = 10'000;
    constexpr uint64_t kIterations = 1'000'000;

    argentum::risk::RiskLimits limits{};
    limits.max_order_value = 100'000.0;
    limits.max_position_exposure = 50'000'000.0;
    limits.max_daily_loss = 0.0;  // kill switch out of scope for this measurement

    auto risk = std::make_unique<argentum::risk::RiskManager>(limits);

    uint64_t next_order_id = 1;
    uint64_t accepted = 0;
    uint64_t rejected = 0;
    Order last_accepted{};
    bool has_last_accepted = false;

    const argentum::core::LatencyReport report = argentum::benchmark::measure_loop(
        kWarmup, kIterations, [&](uint64_t i) {
            const Order order = make_order(next_order_id++, static_cast<size_t>(i));
            if (risk->check_order(order)) {
                ++accepted;
                // Cancel the previously accepted order (outside no timing
                // concern: on_cancel is part of the loop body and therefore
                // measured; documented in `note` below).
                if (has_last_accepted) {
                    risk->on_cancel(last_accepted);
                }
                last_accepted = order;
                has_last_accepted = true;
            } else {
                ++rejected;
            }
        });

    argentum::benchmark::BenchmarkReporter reporter("risk_benchmark");
    argentum::benchmark::BenchmarkCase result;
    result.name = "risk_check_order_mixed";
    result.warmup_iterations = kWarmup;
    result.iterations = kIterations;
    result.latency = report;
    result.extra.emplace_back("accepted", std::to_string(accepted));
    result.extra.emplace_back("rejected", std::to_string(rejected));
    result.extra.emplace_back("symbols", "4");
    result.extra.emplace_back(
        "note", "loop body = check_order + on_cancel of previous accept (ledger occupancy stays ~1)");
    reporter.add(result);

    reporter.print_human(stdout);
    const char* json_path = (argc > 1) ? argv[1] : "benchmarks_out/risk.json";
    if (!reporter.write_json(json_path)) {
        std::fprintf(stderr, "[risk_benchmark] failed to write %s\n", json_path);
        return 1;
    }
    std::printf("[risk_benchmark] json: %s\n", json_path);
    return 0;
}
