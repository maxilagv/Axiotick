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

constexpr const char* kStrategy = "lifecycle_probe";
constexpr const char* kSymbol = "BTC/USDT";

void feed_eval(StrategyRegistry& registry, uint64_t id, double realized_bps,
               double notional, uint64_t* ts) {
    registry.record_signal_open(
        id, kStrategy, kSymbol, SIDE_BUY, 100.0, notional,
        0.0, 0.0, argentum::regime::RegimeLabel::Unknown, *ts);
    *ts += 1'000'000'000ULL;
    registry.on_market_tick(kSymbol, 100.0 * (1.0 + realized_bps / 10'000.0), *ts);
    *ts += 1'000'000'000ULL;
}

StrategyRegistryConfig everything_disabled() {
    StrategyRegistryConfig config{};
    config.cusum_lambda_bps = 1e9;
    config.sharpe_window = 1'000'000;
    config.ev_decay_min_samples = 1'000'000;
    config.max_strategy_drawdown_notional = 0.0;
    return config;
}

void test_drawdown_quarantines_directly_from_active() {
    StrategyRegistryConfig config = everything_disabled();
    config.max_strategy_drawdown_notional = 50.0;
    StrategyRegistry registry(config);
    uint64_t ts = 1'000'000'000ULL;

    // +100bps on 10k notional -> cum +100, peak 100.
    feed_eval(registry, 1, +100.0, 10'000.0, &ts);
    CHECK(registry.state(kStrategy) == StrategyState::Active);
    CHECK_NEAR(registry.health(kStrategy).cum_pnl, 100.0, 1e-9);

    // -100bps on 10k -> cum 0, drawdown 100 > 50 -> straight to Quarantined:
    // capital protection does NOT wait in UnderObservation.
    feed_eval(registry, 2, -100.0, 10'000.0, &ts);
    CHECK(registry.state(kStrategy) == StrategyState::Quarantined);

    const auto transitions = registry.transition_history(kStrategy);
    CHECK(transitions.size() == 1);
    CHECK(transitions[0].from == StrategyState::Active);
    CHECK(transitions[0].to == StrategyState::Quarantined);
    CHECK(transitions[0].reason == "strategy_drawdown_breach");
}

void test_recovery_after_clean_streak() {
    StrategyRegistryConfig config = everything_disabled();
    config.recovery_clean_evaluations = 3;
    StrategyRegistry registry(config);
    uint64_t ts = 1'000'000'000ULL;

    registry.force_state(kStrategy, StrategyState::UnderObservation, "test_setup");

    feed_eval(registry, 10, +1.0, 1'000.0, &ts);
    feed_eval(registry, 11, +1.0, 1'000.0, &ts);
    CHECK(registry.state(kStrategy) == StrategyState::UnderObservation);
    CHECK(registry.health(kStrategy).clean_evaluations == 2);

    feed_eval(registry, 12, +1.0, 1'000.0, &ts);
    CHECK(registry.state(kStrategy) == StrategyState::Active);

    const auto transitions = registry.transition_history(kStrategy);
    CHECK(transitions.size() == 2);
    CHECK(transitions[1].reason == "recovery");
}

void test_quarantine_has_no_automatic_exit() {
    StrategyRegistry registry(everything_disabled());
    uint64_t ts = 1'000'000'000ULL;

    registry.force_state(kStrategy, StrategyState::Quarantined, "test_setup");

    // Even a long winning streak must not re-enable a quarantined strategy.
    for (uint64_t i = 0; i < 30; ++i) {
        feed_eval(registry, 100 + i, +50.0, 1'000.0, &ts);
    }
    CHECK(registry.state(kStrategy) == StrategyState::Quarantined);

    // Forensics still work: evaluations kept resolving while quarantined.
    CHECK(registry.resolved_history(kStrategy).size() == 30);

    // Only the manual path re-enables.
    registry.force_state(kStrategy, StrategyState::Active, "operator_review");
    CHECK(registry.state(kStrategy) == StrategyState::Active);
}

void test_repeated_quarantines_disable_automatically() {
    StrategyRegistryConfig config = everything_disabled();
    config.max_strategy_drawdown_notional = 50.0;
    config.max_quarantines_in_lookback = 3;
    StrategyRegistry registry(config);
    uint64_t ts = 1'000'000'000ULL;

    // Cycle 1: gain then breach -> quarantine #1.
    feed_eval(registry, 1, +100.0, 10'000.0, &ts);
    feed_eval(registry, 2, -100.0, 10'000.0, &ts);
    CHECK(registry.state(kStrategy) == StrategyState::Quarantined);

    // Operator re-enables; the drawdown (peak 100 vs cum 0) still stands, so
    // the next resolved evaluation re-breaches -> quarantine #2.
    registry.force_state(kStrategy, StrategyState::Active, "retry_1");
    feed_eval(registry, 3, +1.0, 100.0, &ts);
    CHECK(registry.state(kStrategy) == StrategyState::Quarantined);

    // Third strike escalates automatically to Disabled — the only automatic
    // path into Disabled.
    registry.force_state(kStrategy, StrategyState::Active, "retry_2");
    feed_eval(registry, 4, +1.0, 100.0, &ts);
    CHECK(registry.state(kStrategy) == StrategyState::Disabled);

    const auto transitions = registry.transition_history(kStrategy);
    bool saw_disable = false;
    for (const auto& transition : transitions) {
        if (transition.to == StrategyState::Disabled) {
            CHECK(transition.reason == "repeated_quarantines");
            saw_disable = true;
        }
    }
    CHECK(saw_disable);
    CHECK(registry.health(kStrategy).quarantines_in_lookback == 3);
}

void test_force_same_state_is_noop() {
    StrategyRegistry registry(everything_disabled());
    registry.force_state(kStrategy, StrategyState::Active, "noop");
    CHECK(registry.transition_history(kStrategy).empty());
}

} // namespace

int main() {
    test_drawdown_quarantines_directly_from_active();
    test_recovery_after_clean_streak();
    test_quarantine_has_no_automatic_exit();
    test_repeated_quarantines_disable_automatically();
    test_force_same_state_is_noop();

    if (g_failures != 0) {
        std::printf("strategy_registry_lifecycle_test FAILED (%d checks)\n", g_failures);
        return 1;
    }
    std::printf("strategy_registry_lifecycle_test passed\n");
    return 0;
}
