#include "backtest/backtest_metrics.hpp"

#include "core/fixed_point.hpp"
#include "core/types.h"
#include "risk/historical_var.hpp"

#include <algorithm>
#include <cmath>
#include <random>

namespace argentum::backtest {

namespace {

double percentile_of_sorted(const std::vector<double>& sorted, double q) {
    if (sorted.empty()) return 0.0;
    const size_t index = static_cast<size_t>(q * static_cast<double>(sorted.size() - 1));
    return sorted[index];
}

EquityCurveMetrics percentile_metrics(
    std::vector<double> pnl,
    std::vector<double> drawdown,
    std::vector<double> sharpe,
    std::vector<double> sortino,
    std::vector<double> var95,
    std::vector<double> cvar95,
    double q) {
    std::sort(pnl.begin(), pnl.end());
    std::sort(drawdown.begin(), drawdown.end());
    std::sort(sharpe.begin(), sharpe.end());
    std::sort(sortino.begin(), sortino.end());
    std::sort(var95.begin(), var95.end());
    std::sort(cvar95.begin(), cvar95.end());

    EquityCurveMetrics out{};
    out.total_pnl = percentile_of_sorted(pnl, q);
    out.max_drawdown = percentile_of_sorted(drawdown, q);
    out.sharpe_ratio = percentile_of_sorted(sharpe, q);
    out.sortino_ratio = percentile_of_sorted(sortino, q);
    out.historical_var_95 = percentile_of_sorted(var95, q);
    out.historical_cvar_95 = percentile_of_sorted(cvar95, q);
    return out;
}

} // namespace

EquityCurveMetrics compute_equity_curve_metrics(
    const std::vector<HistoricalTrade>& trades,
    double initial_capital) {
    EquityCurveMetrics metrics{};
    if (trades.empty()) {
        return metrics;
    }

    double cash = initial_capital;
    int64_t position_lots = 0;
    double mark_price = 0.0;
    double max_equity = initial_capital;
    double max_drawdown = 0.0;
    std::vector<double> equity_path;
    equity_path.reserve(trades.size());

    for (const HistoricalTrade& trade : trades) {
        const double px = core::from_price_ticks(trade.price_ticks);
        const double qty = core::from_quantity_lots(trade.quantity_lots);

        if (trade.side == SIDE_BUY) {
            cash -= px * qty;
            position_lots += trade.quantity_lots;
        } else if (trade.side == SIDE_SELL) {
            cash += px * qty;
            position_lots -= trade.quantity_lots;
        }

        mark_price = px;
        const double equity = cash + core::from_quantity_lots(position_lots) * mark_price;
        max_equity = std::max(max_equity, equity);
        if (max_equity > 0.0) {
            max_drawdown = std::max(max_drawdown, (max_equity - equity) / max_equity);
        }
        equity_path.push_back(equity);
    }

    const double final_equity = cash + core::from_quantity_lots(position_lots) * mark_price;
    metrics.total_pnl = final_equity - initial_capital;
    metrics.max_drawdown = max_drawdown;

    if (equity_path.size() > 1) {
        std::vector<double> returns;
        returns.reserve(equity_path.size() - 1);
        for (size_t i = 1; i < equity_path.size(); ++i) {
            returns.push_back(equity_path[i] - equity_path[i - 1]);
        }

        const double n = static_cast<double>(returns.size());
        double sum = 0.0;
        for (double r : returns) sum += r;
        const double mean = sum / n;

        double variance = 0.0;
        double downside_sq_sum = 0.0;
        for (double r : returns) {
            const double d = r - mean;
            variance += d * d;
            if (r < 0.0) {
                downside_sq_sum += r * r;  // MAR = 0 target semideviation over all n
            }
        }
        variance /= n;

        const double stddev = std::sqrt(variance);
        if (stddev > 1e-12) {
            metrics.sharpe_ratio = (mean / stddev) * std::sqrt(252.0);
        }

        const double downside_dev = std::sqrt(downside_sq_sum / n);
        if (downside_dev > 1e-12) {
            metrics.sortino_ratio = (mean / downside_dev) * std::sqrt(252.0);
        }

        const risk::HistoricalVarResult var = risk::historical_var_cvar(returns, 0.95);
        metrics.historical_var_95 = var.var;
        metrics.historical_cvar_95 = var.cvar;
    }

    return metrics;
}

MonteCarloReport run_monte_carlo_bootstrap(
    const std::vector<HistoricalTrade>& trades,
    double initial_capital,
    size_t resamples,
    uint64_t seed) {
    MonteCarloReport report{};
    if (trades.empty() || resamples == 0) {
        return report;
    }
    report.resamples = resamples;

    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<size_t> pick(0, trades.size() - 1);

    std::vector<double> pnl, drawdown, sharpe, sortino, var95, cvar95;
    pnl.reserve(resamples);
    drawdown.reserve(resamples);
    sharpe.reserve(resamples);
    sortino.reserve(resamples);
    var95.reserve(resamples);
    cvar95.reserve(resamples);

    std::vector<HistoricalTrade> resampled(trades.size());
    for (size_t b = 0; b < resamples; ++b) {
        for (size_t i = 0; i < trades.size(); ++i) {
            resampled[i] = trades[pick(rng)];
        }
        const EquityCurveMetrics m = compute_equity_curve_metrics(resampled, initial_capital);
        pnl.push_back(m.total_pnl);
        drawdown.push_back(m.max_drawdown);
        sharpe.push_back(m.sharpe_ratio);
        sortino.push_back(m.sortino_ratio);
        var95.push_back(m.historical_var_95);
        cvar95.push_back(m.historical_cvar_95);
    }

    report.p5 = percentile_metrics(pnl, drawdown, sharpe, sortino, var95, cvar95, 0.05);
    report.p50 = percentile_metrics(pnl, drawdown, sharpe, sortino, var95, cvar95, 0.50);
    report.p95 = percentile_metrics(pnl, drawdown, sharpe, sortino, var95, cvar95, 0.95);
    return report;
}

} // namespace argentum::backtest
