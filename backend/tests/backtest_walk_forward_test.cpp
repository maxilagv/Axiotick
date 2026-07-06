#include "backtest/backtest_engine.hpp"

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

using namespace argentum;

namespace {

constexpr const char* kSymbol = "BTC/USDT";
constexpr uint64_t kSecond = 1'000'000'000ULL;

// Emits a strong-EV BUY candidate on its first K on_tick calls ONLY, then
// goes silent forever. This makes state leakage across folds directly
// observable: with a FRESH instance per fold every fold produces K
// candidates; with a REUSED instance only the first fold would.
class FirstTicksOnlyStrategy : public signal::CandidateStrategy {
public:
    explicit FirstTicksOnlyStrategy(int first_k) : first_k_(first_k) {}

    std::optional<signal::SignalCandidate> on_tick(const MarketTick& tick) override {
        if (emitted_ >= first_k_) {
            return std::nullopt;
        }
        ++emitted_;

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

    [[nodiscard]] std::string strategy_id() const override { return "first_ticks_probe"; }

private:
    int first_k_;
    int emitted_ = 0;
};

std::vector<MarketTick> make_ticks(size_t count) {
    std::vector<MarketTick> ticks;
    ticks.reserve(count);

    double price = 50'000.0;
    uint64_t ts = 1'700'000'000'000'000'000ULL;

    for (size_t i = 0; i < count; ++i) {
        price *= (1.0 + ((i % 2 == 0) ? 1.0 : -1.0) * 0.5 / 10'000.0);
        ts += 250'000'000ULL;  // 4 ticks/second

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

void test_fresh_instance_per_fold_and_coverage() {
    backtest::BacktestEngine engine;
    engine.load_ticks(make_ticks(4'000));  // spans 1000 seconds

    backtest::StrategyBacktestConfig config{};
    config.symbol = kSymbol;
    config.journal_path = "data/test_walk_forward_events.jsonl";
    config.bar_duration_ns = 1'000'000'000ULL;
    config.gate.min_ev_bps_threshold = 3.0;
    config.gate.min_confidence = 0.55;
    config.risk_limits = risk::RiskLimits{1e9, 1e9, 0.0};

    backtest::WalkForwardConfig wf{};
    wf.fold_test_duration_ns = 250 * kSecond;  // 4 non-overlapping folds

    const backtest::WalkForwardReport report = engine.run_walk_forward(
        [] { return std::make_shared<FirstTicksOnlyStrategy>(3); },
        config,
        wf);

    CHECK(report.folds.size() == 4);

    uint64_t total_candidates = 0;
    size_t total_ticks = 0;
    size_t total_fills = 0;
    for (size_t i = 0; i < report.folds.size(); ++i) {
        const auto& fold = report.folds[i];
        // THE state-leak assertion: every fold gets a fresh instance, so
        // every fold produces exactly 3 candidates. A reused instance would
        // produce 3 in fold 0 and 0 afterwards.
        CHECK(fold.report.funnel.candidates == 3);
        CHECK(fold.report.funnel.orders_accepted == 3);
        total_candidates += fold.report.funnel.candidates;
        total_ticks += fold.report.ticks_processed;
        total_fills += fold.report.trades_loaded;

        if (i + 1 < report.folds.size()) {
            CHECK(fold.fold.test_end_ns == report.folds[i + 1].fold.test_start_ns);
        }
    }
    CHECK(total_candidates == 12);
    CHECK(total_ticks == 4'000);  // folds cover the series exactly, no gaps/overlap

    CHECK(report.aggregate_trades == total_fills);
    CHECK(report.aggregate_trades == 12);
    CHECK(std::isfinite(report.aggregate_out_of_sample.total_pnl));
    CHECK(report.aggregate_out_of_sample.max_drawdown >= 0.0);
    CHECK(report.aggregate_out_of_sample.max_drawdown <= 1.0);
}

void test_guards() {
    backtest::BacktestEngine engine;
    engine.load_ticks(make_ticks(100));

    backtest::StrategyBacktestConfig config{};
    config.symbol = kSymbol;
    config.risk_limits = risk::RiskLimits{1e9, 1e9, 0.0};

    // Zero fold duration -> empty report, no crash.
    backtest::WalkForwardConfig bad{};
    const auto empty = engine.run_walk_forward(
        [] { return std::make_shared<FirstTicksOnlyStrategy>(1); }, config, bad);
    CHECK(empty.folds.empty());
    CHECK(empty.aggregate_trades == 0);

    // Null factory -> empty report.
    backtest::WalkForwardConfig ok{};
    ok.fold_test_duration_ns = 10 * kSecond;
    const auto no_factory = engine.run_walk_forward(nullptr, config, ok);
    CHECK(no_factory.folds.empty());
}

} // namespace

int main() {
    test_fresh_instance_per_fold_and_coverage();
    test_guards();

    if (g_failures != 0) {
        std::printf("backtest_walk_forward_test FAILED (%d checks)\n", g_failures);
        return 1;
    }
    std::printf("backtest_walk_forward_test passed\n");
    return 0;
}
