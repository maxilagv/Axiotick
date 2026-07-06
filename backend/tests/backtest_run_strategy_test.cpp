#include "backtest/backtest_engine.hpp"

#include "regime/regime_types.hpp"
#include "signal/candidate_strategy.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
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
constexpr const char* kJournalPath = "data/test_backtest_strategy_events.jsonl";

// Deterministic long-only probe: a strong-EV BUY candidate every N ticks.
// Regime and confidence are stamped by the backtest runner, so pre-warmup
// candidates carry Unknown and post-warmup ones carry the classified label.
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

    void on_regime_change(regime::RegimeLabel label, double confidence) override {
        (void)confidence;
        ++regime_changes_;
        last_label_ = label;
    }

    [[nodiscard]] std::string strategy_id() const override { return "every_nth_probe"; }

    int regime_changes_ = 0;
    regime::RegimeLabel last_label_ = regime::RegimeLabel::Unknown;

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
        price *= (1.0 + 2.0 / 10'000.0);  // steady +2bps per tick: unambiguous trend
        ts += 250'000'000ULL;             // 4 ticks per 1s bar

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

bool journal_has_trade_with_signal_id(const std::string& path) {
    std::ifstream in(path);
    if (!in.is_open()) return false;
    std::string line;
    while (std::getline(in, line)) {
        if (line.find("\"type\":\"trade_executed\"") == std::string::npos) continue;
        const size_t pos = line.find("\"related_signal_id\":");
        if (pos == std::string::npos) continue;
        const char digit = line[pos + std::strlen("\"related_signal_id\":")];
        if (digit >= '1' && digit <= '9') return true;
    }
    return false;
}

} // namespace

int main() {
    backtest::BacktestEngine engine;
    engine.load_ticks(make_trend_ticks(4'000));
    CHECK(engine.loaded_tick_count() == 4'000);

    backtest::StrategyBacktestConfig config{};
    config.symbol = kSymbol;
    config.journal_path = kJournalPath;
    config.bar_duration_ns = 1'000'000'000ULL;  // 1s bars: classifier warms up within the test window
    config.gate.min_ev_bps_threshold = 3.0;
    config.gate.min_confidence = 0.55;
    // Excluding Unknown proves the runner stamps regimes onto candidates:
    // pre-warmup candidates (Unknown) MUST be rejected regime_blocked, and
    // post-warmup ones (Trend) MUST pass.
    config.gate.allowed_regimes_mask =
        regime::kAllRegimesMask & ~regime::regime_bit(regime::RegimeLabel::Unknown);
    config.risk_limits = risk::RiskLimits{1e9, 1e9, 0.0};

    auto strategy = std::make_shared<EveryNthTickBuyStrategy>(10);
    const backtest::StrategyBacktestReport report = engine.run_strategy(strategy, config);

    CHECK(report.ticks_processed == 4'000);
    CHECK(report.bars_closed >= 990);  // ~999 one-second bars from 4000 ticks at 250ms

    CHECK(report.funnel.candidates == 400);
    const uint64_t regime_blocked = report.funnel.rejects_by_reason[
        static_cast<size_t>(ev::RejectReason::RegimeBlocked)];
    CHECK(regime_blocked > 0);                  // pre-warmup candidates carried Unknown
    CHECK(report.funnel.gate_accepted > 300);   // post-warmup candidates carried Trend
    CHECK(report.funnel.orders_accepted == report.funnel.gate_accepted);
    CHECK(report.funnel.candidates ==
          report.funnel.gate_accepted + regime_blocked);

    // Regime distribution: warmup Unknown bars first, then a solid trend.
    const uint64_t unknown_bars = report.regime_bar_counts[
        static_cast<size_t>(regime::RegimeLabel::Unknown)];
    const uint64_t trend_bars = report.regime_bar_counts[
        static_cast<size_t>(regime::RegimeLabel::Trend)];
    CHECK(unknown_bars > 0);
    CHECK(trend_bars > 800);
    uint64_t counted = 0;
    for (uint64_t c : report.regime_bar_counts) counted += c;
    CHECK(counted == report.bars_closed);
    CHECK(strategy->regime_changes_ >= 1);
    CHECK(strategy->last_label_ == regime::RegimeLabel::Trend);

    // Every accepted order fully fills against the seeded maker in one trade.
    CHECK(report.trades_loaded == report.funnel.orders_accepted);

    // Metrics must be well-formed.
    CHECK(std::isfinite(report.metrics.total_pnl));
    CHECK(std::isfinite(report.metrics.sharpe_ratio));
    CHECK(std::isfinite(report.metrics.sortino_ratio));
    CHECK(report.metrics.max_drawdown >= 0.0);
    CHECK(report.metrics.max_drawdown <= 1.0);
    CHECK(report.metrics.historical_var_95 >= 0.0);
    CHECK(report.metrics.historical_cvar_95 >= report.metrics.historical_var_95);

    // The journal carries the signal correlation for replay.
    CHECK(journal_has_trade_with_signal_id(kJournalPath));

    if (g_failures != 0) {
        std::printf("backtest_run_strategy_test FAILED (%d checks)\n", g_failures);
        return 1;
    }
    std::printf("backtest_run_strategy_test passed\n");
    return 0;
}
