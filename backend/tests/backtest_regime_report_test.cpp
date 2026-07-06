#include "backtest/backtest_engine.hpp"

#include "regime/regime_types.hpp"
#include "signal/candidate_strategy.hpp"
#include "strategy/strategy_registry.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

static int g_failures = 0;
#define CHECK(cond) \
    do { \
        if (!(cond)) { \
            std::printf("CHECK FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures; \
        } \
    } while (0)
#define CHECK_NEAR(a, b, eps) CHECK(std::abs((a) - (b)) <= (eps))

using namespace argentum;

namespace {

constexpr const char* kSymbol = "BTC/USDT";
constexpr uint64_t kHourNs = 3'600ULL * 1'000'000'000ULL;

// The core attribution property, at registry level: an evaluation opened in
// one regime keeps that regime label even when it RESOLVES much later (when
// the market may be in a different regime). Naive fill-filtering by
// resolution-time regime would split entry and exit across buckets.
void test_signal_time_regime_attribution() {
    strategy::StrategyRegistry registry{strategy::StrategyRegistryConfig{}};
    const uint64_t t0 = 1'000ULL * kHourNs;

    registry.record_signal_open(1, "s", kSymbol, SIDE_BUY, 100.0, 1'000.0, 5.0,
                                2.0,  // resolves 2h later, market phase long gone
                                regime::RegimeLabel::Trend, t0);
    registry.on_market_tick(kSymbol, 101.0, t0 + 3 * kHourNs);

    const auto resolved = registry.resolved_history("s");
    CHECK(resolved.size() == 1);
    CHECK(resolved[0].regime == regime::RegimeLabel::Trend);  // signal-time label
    CHECK(resolved[0].resolved_ts_ns == t0 + 3 * kHourNs);
}

class EveryNthTickBuyStrategy : public signal::CandidateStrategy {
public:
    explicit EveryNthTickBuyStrategy(int every) : every_(every) {}

    std::optional<signal::SignalCandidate> on_tick(const MarketTick& tick) override {
        if (++counter_ % every_ != 0) {
            return std::nullopt;
        }

        signal::SignalCandidate candidate{};
        candidate.strategy_id = strategy_id();
        candidate.model_version = "probe_v1";
        candidate.feature_snapshot_id = "probe";
        candidate.symbol = kSymbol;
        candidate.side = SIDE_BUY;
        candidate.price = tick.price;
        candidate.quantity = 0.01;
        candidate.timestamp_ns = tick.timestamp_ns;
        candidate.ev_inputs.p_win = 0.65;
        candidate.ev_inputs.avg_win_bps = 40.0;
        candidate.ev_inputs.avg_loss_bps = -15.0;
        candidate.ev_inputs.confidence = 0.9;
        candidate.ev_inputs.notional = tick.price * candidate.quantity;
        candidate.ev_inputs.costs.taker_fee_bps = 2.0;
        candidate.ev_inputs.costs.half_spread_bps = 0.5;
        candidate.ev_inputs.costs.slippage_bps = 0.5;
        candidate.ev_inputs.costs.market_impact_bps = 0.3;
        candidate.ev_inputs.costs.expected_holding_hours = 0.01;  // 36s horizon
        return candidate;
    }

    [[nodiscard]] std::string strategy_id() const override { return "regime_report_probe"; }

private:
    int every_;
    int counter_ = 0;
};

std::vector<MarketTick> make_trend_ticks(size_t count) {
    std::vector<MarketTick> ticks;
    ticks.reserve(count);

    double price = 50'000.0;
    uint64_t ts = 1'700'000'000'000'000'000ULL;

    for (size_t i = 0; i < count; ++i) {
        price *= (1.0 + 2.0 / 10'000.0);
        ts += 250'000'000ULL;

        MarketTick tick{};
        tick.timestamp_ns = ts;
        tick.price = price;
        tick.quantity = (i % 2 == 0) ? 1.0 : 1.2;
        std::strncpy(tick.symbol, kSymbol, sizeof(tick.symbol) - 1);
        std::strncpy(tick.source, "TEST", sizeof(tick.source) - 1);
        tick.side = SIDE_BUY;
        ticks.push_back(tick);
    }

    return ticks;
}

void test_by_regime_report_consistency() {
    backtest::BacktestEngine engine;
    engine.load_ticks(make_trend_ticks(4'000));

    backtest::StrategyBacktestConfig config{};
    config.symbol = kSymbol;
    config.journal_path = "data/test_regime_report_events.jsonl";
    config.bar_duration_ns = 1'000'000'000ULL;  // 1s bars: warmup within the window
    config.gate.min_ev_bps_threshold = 3.0;
    config.gate.min_confidence = 0.55;
    config.risk_limits = risk::RiskLimits{1e9, 1e9, 0.0};

    auto strategy = std::make_shared<EveryNthTickBuyStrategy>(10);
    const backtest::StrategyBacktestReport report = engine.run_strategy(strategy, config);

    // Every fill got a horizon evaluation, including tail signals via flush.
    CHECK(report.resolved_evaluations == report.trades_loaded);
    CHECK(report.final_health.pending_count == 0);
    CHECK(report.final_health.resolved_count == report.resolved_evaluations);

    // Bucket totals must add up exactly to the resolved sample.
    uint64_t bucket_total = 0;
    for (const auto& bucket : report.by_regime) {
        bucket_total += bucket.evaluation_count;
        CHECK(bucket.hit_rate >= 0.0);
        CHECK(bucket.hit_rate <= 1.0);
    }
    CHECK(bucket_total == report.resolved_evaluations);

    // Pre-warmup candidates carry Unknown; post-warmup ones carry Trend —
    // both buckets must be populated in this series.
    const auto& unknown = report.by_regime[static_cast<size_t>(regime::RegimeLabel::Unknown)];
    const auto& trend = report.by_regime[static_cast<size_t>(regime::RegimeLabel::Trend)];
    CHECK(unknown.evaluation_count > 0);
    CHECK(trend.evaluation_count > 0);

    // The probe always promises the same EV; the bucket must reflect it.
    CHECK_NEAR(trend.mean_predicted_ev_bps, 14.95, 1e-6);

    // Steady uptrend + long-only probe: the Trend bucket should be winning.
    CHECK(trend.mean_realized_bps > 0.0);
    CHECK(trend.hit_rate > 0.5);
}

} // namespace

int main() {
    test_signal_time_regime_attribution();
    test_by_regime_report_consistency();

    if (g_failures != 0) {
        std::printf("backtest_regime_report_test FAILED (%d checks)\n", g_failures);
        return 1;
    }
    std::printf("backtest_regime_report_test passed\n");
    return 0;
}
