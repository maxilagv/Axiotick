#include "backtest/backtest_metrics.hpp"

#include "core/fixed_point.hpp"
#include "core/types.h"
#include "risk/historical_var.hpp"

#include <cmath>
#include <cstdio>
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

using argentum::backtest::EquityCurveMetrics;
using argentum::backtest::HistoricalTrade;
using argentum::backtest::MonteCarloReport;
using argentum::backtest::compute_equity_curve_metrics;
using argentum::backtest::run_monte_carlo_bootstrap;

namespace {

HistoricalTrade make_fill(uint8_t side, double price, double quantity) {
    return HistoricalTrade{
        argentum::core::to_price_ticks(price),
        argentum::core::to_quantity_lots(quantity),
        side
    };
}

void test_hand_computed_equity_curve() {
    // BUY 1@100 -> equity 100000; SELL 1@110 -> 100010; BUY 1@100 -> 100010;
    // SELL 1@80 -> 99990. Returns: {+10, 0, -20}.
    const std::vector<HistoricalTrade> trades = {
        make_fill(SIDE_BUY, 100.0, 1.0),
        make_fill(SIDE_SELL, 110.0, 1.0),
        make_fill(SIDE_BUY, 100.0, 1.0),
        make_fill(SIDE_SELL, 80.0, 1.0),
    };

    const EquityCurveMetrics m = compute_equity_curve_metrics(trades, 100'000.0);

    CHECK_NEAR(m.total_pnl, -10.0, 1e-9);
    CHECK_NEAR(m.max_drawdown, 20.0 / 100'010.0, 1e-12);

    // mean = -10/3; population stddev = sqrt(4200/27); annualized sqrt(252).
    const double mean = -10.0 / 3.0;
    const double stddev = std::sqrt(4200.0 / 27.0);
    CHECK_NEAR(m.sharpe_ratio, mean / stddev * std::sqrt(252.0), 1e-9);

    // MAR=0 target semideviation over all n returns: sqrt(400/3).
    const double downside = std::sqrt(400.0 / 3.0);
    CHECK_NEAR(m.sortino_ratio, mean / downside * std::sqrt(252.0), 1e-9);

    // Sorted returns [-20, 0, 10]; tail index floor(0.05*2)=0.
    CHECK_NEAR(m.historical_var_95, 20.0, 1e-9);
    CHECK_NEAR(m.historical_cvar_95, 20.0, 1e-9);

    // Empty input -> all zeros.
    const EquityCurveMetrics empty = compute_equity_curve_metrics({}, 100'000.0);
    CHECK_NEAR(empty.total_pnl, 0.0, 1e-12);
    CHECK_NEAR(empty.sharpe_ratio, 0.0, 1e-12);
}

void test_historical_var_cvar() {
    // 5 known losses among 95 gains: tail index floor(0.05*99)=4.
    std::vector<double> returns;
    returns.push_back(-50.0);
    returns.push_back(-40.0);
    returns.push_back(-30.0);
    returns.push_back(-20.0);
    returns.push_back(-10.0);
    for (int i = 1; i <= 95; ++i) {
        returns.push_back(static_cast<double>(i));
    }

    const auto result = argentum::risk::historical_var_cvar(returns, 0.95);
    CHECK_NEAR(result.var, 10.0, 1e-12);
    CHECK_NEAR(result.cvar, 30.0, 1e-12);

    // All-positive sample: clamped to zero (no loss in the tail).
    const auto no_loss = argentum::risk::historical_var_cvar({1.0, 2.0, 3.0}, 0.95);
    CHECK_NEAR(no_loss.var, 0.0, 1e-12);
    CHECK_NEAR(no_loss.cvar, 0.0, 1e-12);

    // Degenerate inputs.
    CHECK_NEAR(argentum::risk::historical_var_cvar({}, 0.95).var, 0.0, 1e-12);
    CHECK_NEAR(argentum::risk::historical_var_cvar({-1.0}, 1.0).var, 0.0, 1e-12);
    CHECK_NEAR(argentum::risk::historical_var_cvar({-1.0}, 0.0).var, 0.0, 1e-12);
}

bool metrics_equal(const EquityCurveMetrics& a, const EquityCurveMetrics& b) {
    return a.total_pnl == b.total_pnl &&
           a.max_drawdown == b.max_drawdown &&
           a.sharpe_ratio == b.sharpe_ratio &&
           a.sortino_ratio == b.sortino_ratio &&
           a.historical_var_95 == b.historical_var_95 &&
           a.historical_cvar_95 == b.historical_cvar_95;
}

void test_monte_carlo_bootstrap() {
    std::vector<HistoricalTrade> trades;
    // Alternating winners/losers of varying size around 100.
    for (int i = 0; i < 10; ++i) {
        const double entry = 100.0;
        const double exit_px = (i % 2 == 0) ? 100.0 + 1.0 + i : 100.0 - 2.0 - i;
        trades.push_back(make_fill(SIDE_BUY, entry, 1.0));
        trades.push_back(make_fill(SIDE_SELL, exit_px, 1.0));
    }

    const MonteCarloReport a = run_monte_carlo_bootstrap(trades, 100'000.0, 200, 42);
    const MonteCarloReport b = run_monte_carlo_bootstrap(trades, 100'000.0, 200, 42);

    CHECK(a.resamples == 200);
    // Bit-identical for identical seed: the determinism contract.
    CHECK(metrics_equal(a.p5, b.p5));
    CHECK(metrics_equal(a.p50, b.p50));
    CHECK(metrics_equal(a.p95, b.p95));

    // Per-metric percentiles must be ordered.
    CHECK(a.p5.total_pnl <= a.p50.total_pnl);
    CHECK(a.p50.total_pnl <= a.p95.total_pnl);
    CHECK(a.p5.max_drawdown <= a.p50.max_drawdown);
    CHECK(a.p50.max_drawdown <= a.p95.max_drawdown);

    // Sanity: everything finite.
    CHECK(std::isfinite(a.p5.total_pnl));
    CHECK(std::isfinite(a.p95.sharpe_ratio));
    CHECK(std::isfinite(a.p95.sortino_ratio));

    // Degenerate inputs.
    const MonteCarloReport empty = run_monte_carlo_bootstrap({}, 100'000.0, 200, 42);
    CHECK(empty.resamples == 0);
    const MonteCarloReport none = run_monte_carlo_bootstrap(trades, 100'000.0, 0, 42);
    CHECK(none.resamples == 0);
}

} // namespace

int main() {
    test_hand_computed_equity_curve();
    test_historical_var_cvar();
    test_monte_carlo_bootstrap();

    if (g_failures != 0) {
        std::printf("backtest_metrics_test FAILED (%d checks)\n", g_failures);
        return 1;
    }
    std::printf("backtest_metrics_test passed\n");
    return 0;
}
