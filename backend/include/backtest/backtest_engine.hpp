#pragma once

#include "core/types.h"
#include "analysis/strategy.hpp"
#include "backtest/backtest_metrics.hpp"
#include "core/pipeline_telemetry.hpp"
#include "ev/ev_gate.hpp"
#include "regime/regime_classifier.hpp"
#include "regime/regime_types.hpp"
#include "risk/risk_manager.hpp"
#include "signal/candidate_strategy.hpp"
#include "signal/signal_engine.hpp"
#include "strategy/strategy_registry_types.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace argentum::backtest {

struct BacktestResult {
    double total_pnl;
    size_t trades_count;
    double max_drawdown;
    double sharpe_ratio;
};

/**
 * @brief Configuration for the counterfactual backtest (Mode B).
 *
 * The run drives the EXACT production decision path — SignalEngine -> EVGate
 * -> OrderManager -> RiskManager -> OrderBook — over historical ticks. The
 * only difference from live is the tick source and the synthetic counterparty
 * liquidity seeded per candidate.
 */
struct StrategyBacktestConfig {
    ev::EVGateConfig gate{};
    risk::RiskLimits risk_limits{};
    regime::RegimeClassifierConfig regime{};
    strategy::StrategyRegistryConfig registry{};           // lifecycle governance thresholds
    strategy::StrategyRegistration registration{};         // baseline evidence (Sharpe floor input)
    std::string symbol;                                    // empty -> taken from the first loaded tick
    uint64_t bar_duration_ns = 60ULL * 1'000'000'000ULL;   // 1m bars
    // Maker size seeded per candidate = candidate size * this multiplier.
    // Seeds AT the reference tick price (no synthetic spread/depth model);
    // cost realism is explored via run_cost_sensitivity instead.
    double synthetic_liquidity_multiplier = 3.0;
    double initial_capital = 100'000.0;
    std::string journal_path = "data/backtest_strategy_events.jsonl";  // scratch artifact, recreated per run
    uint64_t first_signal_id = 1;
    uint64_t first_order_id = 1'000'000;
    uint64_t maker_base_order_id = 900'000'000;
};

/**
 * @brief Per-regime aggregation of horizon evaluations (see ADR 0013 for why
 * this is built from ResolvedEvaluation streams and NOT by filtering fills:
 * a position opened in one regime and closed in another would leave either
 * subset with a phantom leg and misattributed PnL).
 */
struct RegimeMetrics {
    uint64_t evaluation_count = 0;
    double mean_realized_bps = 0.0;
    double mean_predicted_ev_bps = 0.0;
    double hit_rate = 0.0;  // fraction with realized_bps > 0
};

struct StrategyBacktestReport {
    signal::SignalEngineStats funnel{};
    EquityCurveMetrics metrics{};
    size_t ticks_processed = 0;
    size_t bars_closed = 0;
    size_t trades_loaded = 0;      // fills reconstructed from the run's journal
    size_t resolved_evaluations = 0;
    std::array<uint64_t, regime::kRegimeCount> regime_bar_counts{};
    std::array<RegimeMetrics, regime::kRegimeCount> by_regime{};
    std::vector<strategy::TransitionEvent> lifecycle_transitions;
    strategy::StrategyHealthSnapshot final_health{};
    // Mode B runs the exact production decision path, so it also measures it:
    // per-stage and wire-to-decision percentiles from the run's TraceSpans.
    core::PipelineLatencyReport pipeline_latency{};
};

using StrategyFactory = std::function<std::shared_ptr<signal::CandidateStrategy>()>;

/**
 * @brief Time-sliced out-of-sample validation.
 *
 * train_* windows are informative only for now: rule-based strategies do not
 * retrain, so no hyperparameter selection happens per fold yet — that joins
 * the Block 4+ Python model workflow. Do NOT present this as parameter
 * optimization; it is out-of-sample evaluation with fresh state per fold.
 */
struct WalkForwardFold {
    uint64_t train_start_ns = 0;
    uint64_t train_end_ns = 0;
    uint64_t test_start_ns = 0;
    uint64_t test_end_ns = 0;
};

struct WalkForwardConfig {
    uint64_t fold_test_duration_ns = 0;   // required > 0
    uint64_t fold_train_duration_ns = 0;  // reserved for retraining workflows
    uint64_t step_ns = 0;                 // 0 -> equals fold_test_duration_ns (non-overlapping folds)
};

struct WalkForwardFoldResult {
    WalkForwardFold fold{};
    StrategyBacktestReport report{};
};

struct WalkForwardReport {
    std::vector<WalkForwardFoldResult> folds;
    // All folds' fills concatenated chronologically, scored once.
    EquityCurveMetrics aggregate_out_of_sample{};
    size_t aggregate_trades = 0;
};

struct CostSensitivityEntry {
    double cost_multiplier = 1.0;
    StrategyBacktestReport report{};
};

struct CostSensitivityReport {
    std::vector<CostSensitivityEntry> entries;
};

/**
 * @class BacktestEngine
 * @brief Replays historical data to evaluate strategies.
 *
 * Mode A (`run`): equity-curve analysis over already-executed fills loaded
 * from CSV/journal — post-mortem of live/paper sessions.
 * Mode B (`run_strategy`): counterfactual run generating hypothetical orders
 * from a CandidateStrategy through the production decision path.
 */
class BacktestEngine {
public:
    BacktestEngine() = default;

    void load_data(const std::string& symbol, const std::string& start_date, const std::string& end_date);
    bool load_ticks_from_csv(const std::string& csv_path, const std::string& symbol = "");
    bool load_trades_from_journal(const std::string& journal_path);

    /// Replaces the tick history with an in-memory series (sorted by time).
    /// Loaded executed-fill data (`trades_`) is left untouched.
    void load_ticks(std::vector<MarketTick> ticks);

    /**
     * @brief Mode A: metrics over the loaded executed fills.
     */
    BacktestResult run(std::shared_ptr<analysis::Strategy> strategy);

    /**
     * @brief Trade-level bootstrap over the loaded fills (see
     * run_monte_carlo_bootstrap in backtest_metrics.hpp for semantics and the
     * i.i.d. honesty note). Deterministic for a fixed seed.
     */
    [[nodiscard]] MonteCarloReport run_monte_carlo(
        size_t resamples = 1000,
        uint64_t seed = 0xB0075742ULL) const;

    /**
     * @brief Mode B: counterfactual strategy run over the loaded ticks.
     *
     * Per tick: risk day-roll + mark-to-market, bar aggregation -> regime
     * classification, strategy candidate generation; each candidate is
     * stamped with the current regime label/confidence, synthetic maker
     * liquidity is seeded into the book, and the candidate goes through
     * SignalEngine (EV gate -> OMS -> risk -> matching). Fills are then
     * reconstructed from the run's journal and scored with
     * compute_equity_curve_metrics.
     */
    StrategyBacktestReport run_strategy(
        std::shared_ptr<signal::CandidateStrategy> strategy,
        const StrategyBacktestConfig& config);

    /**
     * @brief Walk-forward: non-overlapping (by default) time folds, each run
     * with a FRESH strategy instance from the factory — internal strategy
     * state (moving averages, counters) must never leak across folds, or the
     * out-of-sample independence of each window is silently invalidated.
     * Fold journals are written to "<journal_path>.foldN".
     */
    WalkForwardReport run_walk_forward(
        const StrategyFactory& strategy_factory,
        const StrategyBacktestConfig& config,
        const WalkForwardConfig& wf_config);

    /**
     * @brief Cost sensitivity sweep: wraps a fresh strategy instance in
     * CostScaledStrategy per multiplier and re-runs the counterfactual.
     * Shows at which cost multiple the edge stops surviving the EV gate.
     * Extreme multipliers double as the cost-shock stress test; regime-level
     * stress behavior is already visible in by_regime[Stress].
     */
    CostSensitivityReport run_cost_sensitivity(
        const StrategyFactory& strategy_factory,
        const StrategyBacktestConfig& config,
        const std::vector<double>& multipliers = {0.5, 1.0, 1.5, 2.0, 3.0, 5.0});

    /// Shared JSONL fill parser (appends to *out; returns true if any fill parsed).
    static bool parse_trades_from_journal(
        const std::string& journal_path,
        std::vector<HistoricalTrade>* out);

    [[nodiscard]] size_t loaded_tick_count() const { return history_.size(); }
    [[nodiscard]] size_t loaded_trade_count() const { return trades_.size(); }

private:
    std::vector<MarketTick> history_;
    std::vector<HistoricalTrade> trades_;
    double initial_capital_ = 100000.0;
};

} // namespace argentum::backtest
