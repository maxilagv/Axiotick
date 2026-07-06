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
using argentum::strategy::StrategyRegistration;
using argentum::strategy::StrategyRegistry;
using argentum::strategy::StrategyRegistryConfig;

namespace {

constexpr const char* kStrategy = "probe";
constexpr const char* kSymbol = "BTC/USDT";

void feed_eval(StrategyRegistry& registry, uint64_t id, double realized_bps,
               double predicted_bps, uint64_t* ts) {
    registry.record_signal_open(
        id, kStrategy, kSymbol, SIDE_BUY, 100.0, 1'000.0,
        predicted_bps, 0.0, argentum::regime::RegimeLabel::Unknown, *ts);
    *ts += 1'000'000'000ULL;
    registry.on_market_tick(kSymbol, 100.0 * (1.0 + realized_bps / 10'000.0), *ts);
    *ts += 1'000'000'000ULL;
}

StrategyRegistryConfig sharpe_only_config() {
    StrategyRegistryConfig config{};
    config.cusum_lambda_bps = 1e9;             // disabled
    config.sharpe_window = 10;
    config.sharpe_floor_ratio = 0.5;
    config.ev_decay_min_samples = 1'000'000;   // disabled
    config.max_strategy_drawdown_notional = 0.0;
    return config;
}

void test_sharpe_floor_fires_with_baseline() {
    StrategyRegistry registry(sharpe_only_config());
    registry.register_strategy(kStrategy, StrategyRegistration{/*baseline_sharpe=*/1.0});
    uint64_t ts = 1'000'000'000ULL;

    // Alternating +1/-1: mean 0, stddev 1 -> ratio 0 < 0.5 * baseline(1.0).
    // The alarm can only fire once the window is FULL (10 samples).
    for (uint64_t i = 0; i < 9; ++i) {
        feed_eval(registry, 100 + i, (i % 2 == 0) ? +1.0 : -1.0, 0.0, &ts);
        CHECK(registry.state(kStrategy) == StrategyState::Active);
    }
    feed_eval(registry, 200, -1.0, 0.0, &ts);  // 10th fills the window
    CHECK(registry.state(kStrategy) == StrategyState::UnderObservation);

    const auto transitions = registry.transition_history(kStrategy);
    CHECK(transitions.size() == 1);
    CHECK(transitions[0].reason == "rolling_sharpe_floor");

    const auto health = registry.health(kStrategy);
    CHECK(health.sharpe_window_full);
    CHECK(health.rolling_sharpe_ratio < 0.5);
}

void test_sharpe_floor_disabled_without_baseline() {
    StrategyRegistry registry(sharpe_only_config());
    // No registration: baseline_sharpe = 0 -> the floor test must never fire
    // (there is no evidence to degrade FROM).
    uint64_t ts = 1'000'000'000ULL;
    for (uint64_t i = 0; i < 30; ++i) {
        feed_eval(registry, 300 + i, (i % 2 == 0) ? +1.0 : -1.0, 0.0, &ts);
    }
    CHECK(registry.state(kStrategy) == StrategyState::Active);
    CHECK(registry.transition_history(kStrategy).empty());
}

StrategyRegistryConfig ev_decay_only_config() {
    StrategyRegistryConfig config{};
    config.cusum_lambda_bps = 1e9;
    config.sharpe_window = 1'000'000;
    config.ev_decay_window = 20;
    config.ev_decay_min_samples = 5;
    config.ev_decay_t_threshold = 2.0;
    config.max_strategy_drawdown_notional = 0.0;
    return config;
}

void test_ev_decay_escalates_only_from_under_observation() {
    StrategyRegistry registry(ev_decay_only_config());
    uint64_t ts = 1'000'000'000ULL;

    registry.force_state(kStrategy, StrategyState::UnderObservation, "test_setup");

    // Predicted +10bps, realized -5bps: diff = -15 consistently. With fewer
    // than min_samples the t-test must NOT fire regardless of how extreme
    // the differences look.
    for (uint64_t i = 0; i < 4; ++i) {
        feed_eval(registry, 400 + i, -5.0, +10.0, &ts);
        CHECK(registry.state(kStrategy) == StrategyState::UnderObservation);
    }

    // 5th sample reaches min_samples -> strongly negative t -> Quarantined.
    feed_eval(registry, 410, -5.0, +10.0, &ts);
    CHECK(registry.state(kStrategy) == StrategyState::Quarantined);

    const auto transitions = registry.transition_history(kStrategy);
    CHECK(transitions.size() == 2);  // manual UO, then ev_decay quarantine
    CHECK(transitions[1].reason == "ev_decay");
    CHECK(transitions[1].to == StrategyState::Quarantined);

    const auto health = registry.health(kStrategy);
    CHECK(health.ev_decay_samples >= 5);
    CHECK(health.ev_decay_t_stat < -2.0);
}

void test_ev_decay_does_not_fire_when_promise_is_kept() {
    StrategyRegistry registry(ev_decay_only_config());
    uint64_t ts = 1'000'000'000ULL;
    registry.force_state(kStrategy, StrategyState::UnderObservation, "test_setup");

    // Realized tracks predicted with small symmetric noise: t ~ 0.
    for (uint64_t i = 0; i < 15; ++i) {
        const double noise = (i % 2 == 0) ? +1.0 : -1.0;
        feed_eval(registry, 500 + i, 10.0 + noise, 10.0, &ts);
    }
    CHECK(registry.state(kStrategy) != StrategyState::Quarantined);
}

} // namespace

int main() {
    test_sharpe_floor_fires_with_baseline();
    test_sharpe_floor_disabled_without_baseline();
    test_ev_decay_escalates_only_from_under_observation();
    test_ev_decay_does_not_fire_when_promise_is_kept();

    if (g_failures != 0) {
        std::printf("strategy_registry_sharpe_ev_decay_test FAILED (%d checks)\n", g_failures);
        return 1;
    }
    std::printf("strategy_registry_sharpe_ev_decay_test passed\n");
    return 0;
}
