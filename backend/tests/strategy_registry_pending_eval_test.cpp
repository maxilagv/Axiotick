#include "strategy/strategy_registry.hpp"

#include "core/types.h"

#include <cmath>
#include <cstdio>

static int g_failures = 0;
#define CHECK(cond) \
    do { \
        if (!(cond)) { \
            std::printf("CHECK FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures; \
        } \
    } while (0)
#define CHECK_NEAR(a, b, eps) CHECK(std::abs((a) - (b)) <= (eps))

using argentum::strategy::StrategyRegistry;
using argentum::strategy::StrategyRegistryConfig;

namespace {

constexpr uint64_t kHourNs = 3'600ULL * 1'000'000'000ULL;

void test_symbol_isolation_and_due_times() {
    StrategyRegistry registry{StrategyRegistryConfig{}};
    const uint64_t t0 = 1'000ULL * kHourNs;

    // A: 1h horizon on SYM1; B: immediate horizon on SYM2.
    registry.record_signal_open(1, "s", "SYM1", SIDE_BUY, 100.0, 1'000.0, 5.0, 1.0,
                                argentum::regime::RegimeLabel::Trend, t0);
    registry.record_signal_open(2, "s", "SYM2", SIDE_BUY, 200.0, 1'000.0, 5.0, 0.0,
                                argentum::regime::RegimeLabel::Range, t0);
    CHECK(registry.pending_count() == 2);

    // A SYM1 tick BEFORE A's horizon: nothing resolves.
    registry.on_market_tick("SYM1", 101.0, t0 + kHourNs / 2);
    CHECK(registry.pending_count() == 2);

    // A SYM1 tick after both are due: resolves A only — B is due but belongs
    // to SYM2 and must NOT resolve against SYM1's price.
    registry.on_market_tick("SYM1", 101.0, t0 + 2 * kHourNs);
    CHECK(registry.pending_count() == 1);

    auto resolved = registry.resolved_history("s");
    CHECK(resolved.size() == 1);
    CHECK(resolved[0].signal_id == 1);
    CHECK(resolved[0].symbol == "SYM1");
    CHECK_NEAR(resolved[0].realized_bps, 100.0, 1e-9);  // 100 -> 101
    CHECK(resolved[0].regime == argentum::regime::RegimeLabel::Trend);
    CHECK(resolved[0].resolved_ts_ns == t0 + 2 * kHourNs);

    // SYM2 tick resolves B at SYM2's own price.
    registry.on_market_tick("SYM2", 199.0, t0 + 2 * kHourNs + 1);
    CHECK(registry.pending_count() == 0);
    resolved = registry.resolved_history("s");
    CHECK(resolved.size() == 2);
    CHECK(resolved[1].signal_id == 2);
    CHECK_NEAR(resolved[1].realized_bps, -50.0, 1e-9);  // 200 -> 199 long
}

void test_sell_side_sign() {
    StrategyRegistry registry{StrategyRegistryConfig{}};
    const uint64_t t0 = 1'000ULL * kHourNs;

    registry.record_signal_open(3, "s", "SYM", SIDE_SELL, 100.0, 1'000.0, 5.0, 0.0,
                                argentum::regime::RegimeLabel::Unknown, t0);
    registry.on_market_tick("SYM", 99.0, t0 + 1);

    const auto resolved = registry.resolved_history("s");
    CHECK(resolved.size() == 1);
    // Short: price falling 1% = +100bps realized.
    CHECK_NEAR(resolved[0].realized_bps, 100.0, 1e-9);
}

void test_flush_resolves_unexpired_instead_of_dropping() {
    StrategyRegistry registry{StrategyRegistryConfig{}};
    const uint64_t t0 = 1'000ULL * kHourNs;

    // 100h horizon: no tick in this test ever reaches it.
    registry.record_signal_open(4, "s", "SYM", SIDE_BUY, 100.0, 1'000.0, 5.0, 100.0,
                                argentum::regime::RegimeLabel::Stress, t0);
    registry.on_market_tick("SYM", 105.0, t0 + kHourNs);
    CHECK(registry.pending_count() == 1);  // not due yet

    // End-of-backtest flush: resolves at the flush price/time instead of
    // silently dropping the sample.
    registry.flush_pending("SYM", 102.0, t0 + 2 * kHourNs);
    CHECK(registry.pending_count() == 0);

    const auto resolved = registry.resolved_history("s");
    CHECK(resolved.size() == 1);
    CHECK_NEAR(resolved[0].realized_bps, 200.0, 1e-9);  // 100 -> 102
    CHECK(resolved[0].resolved_ts_ns == t0 + 2 * kHourNs);
    CHECK(resolved[0].regime == argentum::regime::RegimeLabel::Stress);
}

void test_invalid_inputs_ignored() {
    StrategyRegistry registry{StrategyRegistryConfig{}};

    registry.record_signal_open(5, "s", "SYM", SIDE_BUY, 0.0, 1'000.0, 5.0, 0.0,
                                argentum::regime::RegimeLabel::Unknown, 1);  // bad price
    registry.record_signal_open(6, "", "SYM", SIDE_BUY, 100.0, 1'000.0, 5.0, 0.0,
                                argentum::regime::RegimeLabel::Unknown, 1);  // empty strategy
    CHECK(registry.pending_count() == 0);

    registry.record_signal_open(7, "s", "SYM", SIDE_BUY, 100.0, 1'000.0, 5.0, 0.0,
                                argentum::regime::RegimeLabel::Unknown, 1);
    registry.on_market_tick("SYM", -5.0, 10);  // invalid tick price: ignored
    CHECK(registry.pending_count() == 1);
}

} // namespace

int main() {
    test_symbol_isolation_and_due_times();
    test_sell_side_sign();
    test_flush_resolves_unexpired_instead_of_dropping();
    test_invalid_inputs_ignored();

    if (g_failures != 0) {
        std::printf("strategy_registry_pending_eval_test FAILED (%d checks)\n", g_failures);
        return 1;
    }
    std::printf("strategy_registry_pending_eval_test passed\n");
    return 0;
}
