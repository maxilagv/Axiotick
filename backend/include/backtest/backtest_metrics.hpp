#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace argentum::backtest {

/**
 * @brief One executed fill in fixed-point units, as journaled/loaded by the
 * backtest engine. Public so pure metric functions can be tested without
 * touching engine state or files.
 */
struct HistoricalTrade {
    int64_t price_ticks = 0;
    int64_t quantity_lots = 0;
    uint8_t side = 0;
    uint64_t related_signal_id = 0;  // audit correlation; 0 in pre-Block-1 journals
};

struct EquityCurveMetrics {
    double total_pnl = 0.0;
    double max_drawdown = 0.0;       // fraction of peak equity, in [0, 1]
    double sharpe_ratio = 0.0;       // annualized with sqrt(252), same convention as the original run()
    double sortino_ratio = 0.0;      // MAR = 0, target semideviation over all n returns
    double historical_var_95 = 0.0;  // positive-loss convention, per-trade equity changes
    double historical_cvar_95 = 0.0;
};

/**
 * @brief Pure function: replays the equity curve over the given fills exactly
 * like BacktestEngine::run() historically did (cash/position/mark-to-last-
 * trade-price) and computes every metric. Shared by run(), the Monte Carlo
 * resampler and tests — one implementation, no drift between callers.
 */
[[nodiscard]] EquityCurveMetrics compute_equity_curve_metrics(
    const std::vector<HistoricalTrade>& trades,
    double initial_capital);

struct MonteCarloReport {
    size_t resamples = 0;
    EquityCurveMetrics p5;
    EquityCurveMetrics p50;
    EquityCurveMetrics p95;
};

/**
 * @brief Trade-level bootstrap: resamples the fill sequence WITH replacement
 * (same sample size) `resamples` times and reports the P5/P50/P95 of each
 * metric across resamples (percentiles are taken per metric independently —
 * this is not a joint scenario).
 *
 * Honesty note: i.i.d. resampling of fills destroys their temporal
 * autocorrelation. This is a standard robustness bound on sample composition
 * and ordering — it is NOT a market-scenario simulation and must not be
 * presented as one.
 *
 * Deterministic: seeded mt19937_64, identical results for identical
 * (trades, seed, resamples).
 */
[[nodiscard]] MonteCarloReport run_monte_carlo_bootstrap(
    const std::vector<HistoricalTrade>& trades,
    double initial_capital,
    size_t resamples,
    uint64_t seed);

} // namespace argentum::backtest
