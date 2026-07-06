// Advanced backtest workflows built ON TOP of run_strategy (Mode B):
// walk-forward out-of-sample validation and cost-sensitivity sweeps.
// Kept out of backtest_engine.cpp on purpose — one file per responsibility.

#include "backtest/backtest_engine.hpp"

#include "analysis/cost_scaled_strategy.hpp"
#include "audit/logger.hpp"

#include <algorithm>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace argentum::backtest {

WalkForwardReport BacktestEngine::run_walk_forward(
    const StrategyFactory& strategy_factory,
    const StrategyBacktestConfig& config,
    const WalkForwardConfig& wf_config) {
    WalkForwardReport report{};
    if (!strategy_factory || history_.empty() || wf_config.fold_test_duration_ns == 0) {
        return report;
    }

    const uint64_t series_start = history_.front().timestamp_ns;
    const uint64_t series_end = history_.back().timestamp_ns;
    const uint64_t step_ns = (wf_config.step_ns != 0)
        ? wf_config.step_ns
        : wf_config.fold_test_duration_ns;

    std::vector<HistoricalTrade> aggregate_fills;
    size_t fold_index = 0;

    uint64_t test_start = series_start + wf_config.fold_train_duration_ns;
    while (test_start <= series_end) {
        const uint64_t test_end = test_start + wf_config.fold_test_duration_ns;

        std::vector<MarketTick> slice;
        for (const MarketTick& tick : history_) {
            if (tick.timestamp_ns >= test_start && tick.timestamp_ns < test_end) {
                slice.push_back(tick);
            }
        }

        if (!slice.empty()) {
            WalkForwardFoldResult fold_result{};
            fold_result.fold.train_start_ns =
                (test_start >= wf_config.fold_train_duration_ns)
                    ? test_start - wf_config.fold_train_duration_ns
                    : 0;
            fold_result.fold.train_end_ns = test_start;
            fold_result.fold.test_start_ns = test_start;
            fold_result.fold.test_end_ns = test_end;

            // Fresh engine per fold: fresh OrderBook/Risk/SignalEngine/registry
            // come for free from run_strategy; the fresh strategy instance
            // comes from the factory (state MUST NOT leak across folds).
            BacktestEngine fold_engine;
            fold_engine.load_ticks(std::move(slice));

            StrategyBacktestConfig fold_config = config;
            char suffix[32];
            std::snprintf(suffix, sizeof(suffix), ".fold%zu", fold_index);
            fold_config.journal_path = config.journal_path + suffix;

            fold_result.report = fold_engine.run_strategy(strategy_factory(), fold_config);

            // Folds run in chronological order, so appending keeps the
            // aggregate fill list chronological too.
            (void)parse_trades_from_journal(fold_config.journal_path, &aggregate_fills);
            report.folds.push_back(std::move(fold_result));
            ++fold_index;
        }

        if (series_end - test_start < step_ns) {
            break;  // no room for another fold start (also avoids u64 wrap)
        }
        test_start += step_ns;
    }

    report.aggregate_trades = aggregate_fills.size();
    report.aggregate_out_of_sample =
        compute_equity_curve_metrics(aggregate_fills, config.initial_capital);

    ARGENTUM_LOG(
        INFO,
        "[Backtest] Walk-forward done folds=" << report.folds.size()
        << " aggregate_trades=" << report.aggregate_trades
        << " oos_pnl=" << report.aggregate_out_of_sample.total_pnl);

    return report;
}

CostSensitivityReport BacktestEngine::run_cost_sensitivity(
    const StrategyFactory& strategy_factory,
    const StrategyBacktestConfig& config,
    const std::vector<double>& multipliers) {
    CostSensitivityReport report{};
    if (!strategy_factory || history_.empty()) {
        return report;
    }

    size_t entry_index = 0;
    for (double multiplier : multipliers) {
        if (!(multiplier > 0.0)) {
            continue;
        }

        CostSensitivityEntry entry{};
        entry.cost_multiplier = multiplier;

        StrategyBacktestConfig sweep_config = config;
        char suffix[32];
        std::snprintf(suffix, sizeof(suffix), ".cost%zu", entry_index);
        sweep_config.journal_path = config.journal_path + suffix;

        auto scaled = std::make_shared<analysis::CostScaledStrategy>(strategy_factory(), multiplier);
        entry.report = run_strategy(scaled, sweep_config);

        report.entries.push_back(std::move(entry));
        ++entry_index;
    }

    return report;
}

} // namespace argentum::backtest
