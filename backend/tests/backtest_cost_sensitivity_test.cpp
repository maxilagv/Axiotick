#include "backtest/backtest_engine.hpp"

#include "analysis/cost_scaled_strategy.hpp"
#include "signal/candidate_strategy.hpp"

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

// Fixed-EV probe: gross = 0.65*40 + 0.35*(-15) = 20.75 bps; base costs
// 2*2 + 2*0.5 + 0.5 + 0.3 = 5.8 bps. At cost multiplier m the gate sees
// net = 20.75 - 5.8m, so with threshold 3.0 the edge survives while
// m <= (20.75-3)/5.8 ~= 3.06 and dies at m = 5.
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
        return candidate;
    }

    [[nodiscard]] std::string strategy_id() const override { return "cost_probe"; }

private:
    int every_;
    int counter_ = 0;
};

std::vector<MarketTick> make_ticks(size_t count) {
    std::vector<MarketTick> ticks;
    ticks.reserve(count);

    double price = 50'000.0;
    uint64_t ts = 1'700'000'000'000'000'000ULL;

    for (size_t i = 0; i < count; ++i) {
        price *= (1.0 + ((i % 2 == 0) ? 1.0 : -1.0) * 0.5 / 10'000.0);
        ts += 250'000'000ULL;

        MarketTick tick{};
        tick.timestamp_ns = ts;
        tick.price = price;
        tick.quantity = 1.0;
        std::strncpy(tick.symbol, kSymbol, sizeof(tick.symbol) - 1);
        std::strncpy(tick.source, "TEST", sizeof(tick.source) - 1);
        tick.side = SIDE_BUY;
        ticks.push_back(tick);
    }

    return ticks;
}

void test_decorator_scales_costs_only() {
    auto inner = std::make_shared<EveryNthTickBuyStrategy>(1);
    analysis::CostScaledStrategy scaled(inner, 2.0);

    MarketTick tick{};
    tick.timestamp_ns = 1;
    tick.price = 100.0;
    tick.quantity = 1.0;
    std::strncpy(tick.symbol, kSymbol, sizeof(tick.symbol) - 1);
    tick.side = SIDE_BUY;

    const auto candidate = scaled.on_tick(tick);
    CHECK(candidate.has_value());
    CHECK_NEAR(candidate->ev_inputs.costs.taker_fee_bps, 4.0, 1e-12);
    CHECK_NEAR(candidate->ev_inputs.costs.half_spread_bps, 1.0, 1e-12);
    CHECK_NEAR(candidate->ev_inputs.costs.slippage_bps, 1.0, 1e-12);
    CHECK_NEAR(candidate->ev_inputs.costs.market_impact_bps, 0.6, 1e-12);
    // Probabilities/moves are NOT costs: untouched.
    CHECK_NEAR(candidate->ev_inputs.p_win, 0.65, 1e-12);
    CHECK_NEAR(candidate->ev_inputs.avg_win_bps, 40.0, 1e-12);
    CHECK(scaled.strategy_id() == "cost_probe_cost_x2.00");
}

void test_sweep_kills_the_edge_monotonically() {
    backtest::BacktestEngine engine;
    engine.load_ticks(make_ticks(2'000));

    backtest::StrategyBacktestConfig config{};
    config.symbol = kSymbol;
    config.journal_path = "data/test_cost_sensitivity_events.jsonl";
    config.bar_duration_ns = 1'000'000'000ULL;
    config.gate.min_ev_bps_threshold = 3.0;
    config.gate.min_confidence = 0.55;
    config.risk_limits = risk::RiskLimits{1e9, 1e9, 0.0};

    const auto factory = [] { return std::make_shared<EveryNthTickBuyStrategy>(10); };

    const backtest::CostSensitivityReport report =
        engine.run_cost_sensitivity(factory, config, {0.5, 1.0, 2.0, 3.0, 5.0});

    CHECK(report.entries.size() == 5);

    // x1.0 must reproduce a plain run of the unwrapped probe exactly (same
    // deterministic data, same gate) — the decorator at multiplier 1 is a
    // pure pass-through cost-wise.
    backtest::StrategyBacktestConfig plain_config = config;
    plain_config.journal_path = config.journal_path + ".plain";
    const backtest::StrategyBacktestReport plain = engine.run_strategy(factory(), plain_config);
    CHECK(report.entries[1].report.funnel.candidates == plain.funnel.candidates);
    CHECK(report.entries[1].report.funnel.gate_accepted == plain.funnel.gate_accepted);
    CHECK(report.entries[1].report.trades_loaded == plain.trades_loaded);

    // Rising costs can only shrink the accepted set.
    for (size_t i = 1; i < report.entries.size(); ++i) {
        CHECK(report.entries[i].report.funnel.gate_accepted <=
              report.entries[i - 1].report.funnel.gate_accepted);
        CHECK(report.entries[i].cost_multiplier > report.entries[i - 1].cost_multiplier);
    }

    // Survives at 3x (net 3.35 >= 3.0), dies at 5x (net negative).
    CHECK(report.entries[3].report.funnel.gate_accepted > 0);
    const auto& dead = report.entries[4].report;
    CHECK(dead.funnel.gate_accepted == 0);
    CHECK(dead.funnel.rejects_by_reason[static_cast<size_t>(ev::RejectReason::EvBelowThreshold)] ==
          dead.funnel.candidates);
}

} // namespace

int main() {
    test_decorator_scales_costs_only();
    test_sweep_kills_the_edge_monotonically();

    if (g_failures != 0) {
        std::printf("backtest_cost_sensitivity_test FAILED (%d checks)\n", g_failures);
        return 1;
    }
    std::printf("backtest_cost_sensitivity_test passed\n");
    return 0;
}
