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

using argentum::ev::StrategyState;
using argentum::strategy::StrategyRegistry;
using argentum::strategy::StrategyRegistryConfig;

namespace {

constexpr const char* kStrategy = "cusum_probe";
constexpr const char* kSymbol = "BTC/USDT";

// Only the Page-Hinkley detector is armed; every other test is disabled so
// this file isolates the CUSUM math exactly.
StrategyRegistryConfig cusum_only_config() {
    StrategyRegistryConfig config{};
    config.cusum_delta_bps = 0.5;
    config.cusum_lambda_bps = 5.0;
    config.sharpe_window = 1'000'000;          // never fills
    config.ev_decay_min_samples = 1'000'000;   // never reached
    config.max_strategy_drawdown_notional = 0.0;
    return config;
}

// Feeds one evaluation with an exact realized_bps: entry at 100, horizon 0,
// resolved by the next tick at 100 * (1 + bps/1e4).
void feed_eval(StrategyRegistry& registry, uint64_t id, double realized_bps, uint64_t* ts) {
    registry.record_signal_open(
        id, kStrategy, kSymbol, SIDE_BUY,
        /*entry_price=*/100.0, /*notional=*/1'000.0,
        /*predicted_ev_net_bps=*/0.0, /*expected_holding_hours=*/0.0,
        argentum::regime::RegimeLabel::Unknown, *ts);
    *ts += 1'000'000'000ULL;
    registry.on_market_tick(kSymbol, 100.0 * (1.0 + realized_bps / 10'000.0), *ts);
    *ts += 1'000'000'000ULL;
}

void test_winning_streak_never_alarms() {
    StrategyRegistry registry(cusum_only_config());
    uint64_t ts = 1'000'000'000ULL;

    for (uint64_t i = 0; i < 50; ++i) {
        feed_eval(registry, 100 + i, +2.0, &ts);
    }

    CHECK(registry.state(kStrategy) == StrategyState::Active);
    CHECK(registry.transition_history(kStrategy).empty());
    // Constant winners: m grows by +delta each step, M tracks m -> PH == 0.
    CHECK_NEAR(registry.health(kStrategy).cusum_ph, 0.0, 1e-9);
}

void test_hand_computed_break_detection() {
    StrategyRegistry registry(cusum_only_config());
    uint64_t ts = 1'000'000'000ULL;

    // 10 winners at exactly +2 bps: mean stays 2, m climbs to 5.0, PH = 0.
    for (uint64_t i = 0; i < 10; ++i) {
        feed_eval(registry, 200 + i, +2.0, &ts);
    }
    CHECK_NEAR(registry.health(kStrategy).cusum_ph, 0.0, 1e-9);
    CHECK(registry.state(kStrategy) == StrategyState::Active);

    // 11th at -4 bps: mean = 16/11; m += (-4 - 16/11 + 0.5) = -4.954545...
    // PH = 5.0 - 0.045454... = 4.954545... — just under lambda = 5.
    feed_eval(registry, 300, -4.0, &ts);
    CHECK(registry.state(kStrategy) == StrategyState::Active);
    CHECK_NEAR(registry.health(kStrategy).cusum_ph, 4.9545454545454545, 1e-9);

    // 12th at -4 bps: mean = 1.0; m += (-4 - 1 + 0.5) -> PH = 9.4545... > 5
    // -> alarm -> Active -> UnderObservation, and the detector resets.
    feed_eval(registry, 301, -4.0, &ts);
    CHECK(registry.state(kStrategy) == StrategyState::UnderObservation);

    const auto transitions = registry.transition_history(kStrategy);
    CHECK(transitions.size() == 1);
    CHECK(transitions[0].from == StrategyState::Active);
    CHECK(transitions[0].to == StrategyState::UnderObservation);
    CHECK(transitions[0].reason == "cusum_alarm");
    CHECK_NEAR(registry.health(kStrategy).cusum_ph, 0.0, 1e-9);  // reset after transition
}

void test_reset_enables_second_detection() {
    StrategyRegistry registry(cusum_only_config());
    uint64_t ts = 1'000'000'000ULL;

    // First break.
    for (uint64_t i = 0; i < 10; ++i) feed_eval(registry, 400 + i, +2.0, &ts);
    feed_eval(registry, 420, -4.0, &ts);
    feed_eval(registry, 421, -4.0, &ts);
    CHECK(registry.state(kStrategy) == StrategyState::UnderObservation);

    // Operator re-enables; the detector was reset, so the SAME pattern must
    // be detectable again from scratch.
    registry.force_state(kStrategy, StrategyState::Active, "test_reenable");
    for (uint64_t i = 0; i < 10; ++i) feed_eval(registry, 500 + i, +2.0, &ts);
    feed_eval(registry, 520, -4.0, &ts);
    feed_eval(registry, 521, -4.0, &ts);
    CHECK(registry.state(kStrategy) == StrategyState::UnderObservation);

    const auto transitions = registry.transition_history(kStrategy);
    CHECK(transitions.size() == 3);  // UO(cusum), Active(manual), UO(cusum)
    CHECK(transitions[2].reason == "cusum_alarm");
}

} // namespace

int main() {
    test_winning_streak_never_alarms();
    test_hand_computed_break_detection();
    test_reset_enables_second_detection();

    if (g_failures != 0) {
        std::printf("strategy_registry_cusum_test FAILED (%d checks)\n", g_failures);
        return 1;
    }
    std::printf("strategy_registry_cusum_test passed\n");
    return 0;
}
