// Block 2 end-to-end harness: counterfactual backtest (Mode B) driving the
// production decision path (SignalEngine -> EVGate -> OMS -> Risk -> Book)
// over historical or synthetic ticks, plus Monte Carlo bootstrap over the
// resulting fills.
//
// Tries data/market_ticks.csv first; falls back to a deterministic synthetic
// series when the CSV has too few ticks to exercise the machinery.

#include "analysis/sma_crossover_strategy.hpp"
#include "backtest/backtest_engine.hpp"
#include "regime/regime_types.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {

constexpr const char* kSymbol = "BTC/USDT";
constexpr const char* kCsvPath = "data/market_ticks.csv";
constexpr const char* kJournalPath = "data/backtest_demo_events.jsonl";
constexpr double kTargetNotional = 2'000.0;
constexpr size_t kMinUsableTicks = 500;

class Lcg {
public:
    explicit Lcg(uint64_t seed) : state_(seed) {}

    double next_unit() {
        state_ = state_ * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<double>((state_ >> 11) & ((1ULL << 53) - 1)) /
               static_cast<double>(1ULL << 53);
    }

    double next_gaussian() {
        const double u1 = std::max(1e-12, next_unit());
        const double u2 = next_unit();
        return std::sqrt(-2.0 * std::log(u1)) * std::cos(6.283185307179586 * u2);
    }

private:
    uint64_t state_;
};

std::vector<MarketTick> synthesize_ticks(size_t count) {
    std::vector<MarketTick> ticks;
    ticks.reserve(count);

    Lcg rng(20261001ULL);
    double price = 50'000.0;
    uint64_t ts = 1'700'000'000'000'000'000ULL;

    for (size_t i = 0; i < count; ++i) {
        // Four phases so the classifier sees trend, range and stress-like vol.
        double drift_bps = 0.0;
        double vol_bps = 5.0;
        double volume_scale = 1.0;
        const size_t phase = (i * 4) / count;
        if (phase == 0) {
            drift_bps = 1.0;
            vol_bps = 5.0;
        } else if (phase == 1) {
            drift_bps = 0.0;
            vol_bps = 1.5;
        } else if (phase == 2) {
            drift_bps = -2.0;
            vol_bps = 12.0;
            volume_scale = 6.0;  // panic volume
        } else {
            drift_bps = 0.6;
            vol_bps = 4.0;
        }

        const double ret_bps = drift_bps + vol_bps * rng.next_gaussian();
        price *= (1.0 + ret_bps / 10'000.0);
        ts += 250'000'000ULL;  // 250ms per tick -> 240 ticks per 1m bar

        MarketTick tick{};
        tick.timestamp_ns = ts;
        tick.price = price;
        tick.quantity = volume_scale * (0.05 + 0.10 * rng.next_unit());
        std::strncpy(tick.symbol, kSymbol, sizeof(tick.symbol) - 1);
        std::strncpy(tick.source, "SYNTH", sizeof(tick.source) - 1);
        tick.side = static_cast<uint8_t>((rng.next_unit() < 0.5) ? SIDE_BUY : SIDE_SELL);
        ticks.push_back(tick);
    }

    return ticks;
}

void print_metrics(const char* label, const argentum::backtest::EquityCurveMetrics& m) {
    std::printf("%-4s pnl=%10.2f dd=%6.4f sharpe=%8.3f sortino=%8.3f var95=%8.3f cvar95=%8.3f\n",
                label, m.total_pnl, m.max_drawdown, m.sharpe_ratio, m.sortino_ratio,
                m.historical_var_95, m.historical_cvar_95);
}

} // namespace

int main() {
    using namespace argentum;

    backtest::BacktestEngine engine;

    bool from_csv = engine.load_ticks_from_csv(kCsvPath, kSymbol);
    if (!from_csv || engine.loaded_tick_count() < kMinUsableTicks) {
        std::printf("[data] %s has %zu usable ticks (<%zu) -> using deterministic synthetic series\n",
                    kCsvPath, engine.loaded_tick_count(), kMinUsableTicks);
        engine.load_ticks(synthesize_ticks(20'000));
        from_csv = false;
    }

    backtest::StrategyBacktestConfig config{};
    config.symbol = kSymbol;
    config.journal_path = kJournalPath;
    config.bar_duration_ns = 60ULL * 1'000'000'000ULL;  // 1m bars
    config.gate.min_ev_bps_threshold = 3.0;
    config.gate.min_confidence = 0.55;
    // Demand double the edge under stress; Unknown stays tradeable for the
    // demo so pre-warmup candidates are visible in the funnel.
    config.gate.regime_threshold_multiplier[static_cast<size_t>(regime::RegimeLabel::Stress)] = 2.0;
    config.risk_limits = risk::RiskLimits{
        100'000.0,    // max_order_value
        500'000.0,    // max_position_exposure
        0.0           // daily loss switch off: the demo shows the full curve, risk drills live in signal_demo
    };
    // Lifecycle governance armed with thresholds tight enough for the demo
    // strategy (a losing SMA cross on this series) to trip them live.
    config.registry.cusum_delta_bps = 0.5;
    config.registry.cusum_lambda_bps = 25.0;
    config.registry.sharpe_window = 30;
    config.registry.ev_decay_window = 50;
    config.registry.ev_decay_min_samples = 20;
    config.registry.ev_decay_t_threshold = 2.0;
    config.registry.max_strategy_drawdown_notional = 60.0;
    config.registration.baseline_sharpe = 0.3;

    // Horizon 0.01h = 36s = ~144 ticks of this series: sized so horizon
    // evaluations measure moves in tens of bps, matching the declared EV
    // scale (the synthetic series compresses time heavily).
    const auto factory = [] {
        return std::make_shared<analysis::SmaCrossoverStrategy>(kSymbol, kTargetNotional, 0.01);
    };

    std::printf("=== Axiotick backtest demo (Block 3: governance + advanced validation) ===\n");
    std::printf("source=%s ticks=%zu symbol=%s journal=%s\n\n",
                from_csv ? kCsvPath : "synthetic", engine.loaded_tick_count(), kSymbol, kJournalPath);

    const backtest::StrategyBacktestReport report = engine.run_strategy(factory(), config);

    std::printf("--- counterfactual run ---\n");
    std::printf("ticks_processed     : %zu\n", report.ticks_processed);
    std::printf("bars_closed         : %zu\n", report.bars_closed);
    std::printf("candidates          : %llu\n", static_cast<unsigned long long>(report.funnel.candidates));
    std::printf("gate_accepted       : %llu\n", static_cast<unsigned long long>(report.funnel.gate_accepted));
    std::printf("orders_accepted     : %llu\n", static_cast<unsigned long long>(report.funnel.orders_accepted));
    for (size_t i = 0; i < report.funnel.rejects_by_reason.size(); ++i) {
        if (report.funnel.rejects_by_reason[i] == 0) continue;
        std::printf("rejected[%-22s]: %llu\n",
                    ev::to_string(static_cast<ev::RejectReason>(i)),
                    static_cast<unsigned long long>(report.funnel.rejects_by_reason[i]));
    }

    std::printf("\n--- lifecycle governance (automatic transitions) ---\n");
    if (report.lifecycle_transitions.empty()) {
        std::printf("(no transitions: strategy stayed %s)\n",
                    ev::to_string(report.final_health.state));
    }
    for (const auto& transition : report.lifecycle_transitions) {
        std::printf("%s -> %s  reason=%s\n",
                    ev::to_string(transition.from),
                    ev::to_string(transition.to),
                    transition.reason.c_str());
    }
    std::printf("final state=%s resolved=%llu pending=%llu cusum_ph=%.2f dd=%.2f\n",
                ev::to_string(report.final_health.state),
                static_cast<unsigned long long>(report.final_health.resolved_count),
                static_cast<unsigned long long>(report.final_health.pending_count),
                report.final_health.cusum_ph,
                report.final_health.current_drawdown);

    std::printf("\n--- regime distribution (bars) ---\n");
    for (size_t i = 0; i < report.regime_bar_counts.size(); ++i) {
        if (report.regime_bar_counts[i] == 0) continue;
        std::printf("%-14s: %llu\n",
                    regime::to_string(static_cast<regime::RegimeLabel>(i)),
                    static_cast<unsigned long long>(report.regime_bar_counts[i]));
    }

    std::printf("\n--- per-regime evaluation report (horizon-based, signal-time attribution) ---\n");
    std::printf("%-14s %8s %14s %15s %8s\n", "regime", "evals", "realized_bps", "predicted_bps", "hit");
    for (size_t i = 0; i < report.by_regime.size(); ++i) {
        const auto& bucket = report.by_regime[i];
        if (bucket.evaluation_count == 0) continue;
        std::printf("%-14s %8llu %14.2f %15.2f %7.0f%%\n",
                    regime::to_string(static_cast<regime::RegimeLabel>(i)),
                    static_cast<unsigned long long>(bucket.evaluation_count),
                    bucket.mean_realized_bps,
                    bucket.mean_predicted_ev_bps,
                    bucket.hit_rate * 100.0);
    }

    std::printf("\n--- strategy metrics (fills=%zu) ---\n", report.trades_loaded);
    print_metrics("run", report.metrics);

    // Mode B exercises the exact production decision path, so its latency IS
    // production decision latency on this host (per-stage, wire-to-decision).
    const argentum::core::PipelineLatencyReport& lat = report.pipeline_latency;
    std::printf("\n--- decision-path latency (Mode B == production path, ticks=%llu) ---\n",
                static_cast<unsigned long long>(lat.ticks_absorbed));
    auto print_latency_row = [](const char* label, const argentum::core::LatencyReport& r) {
        if (r.samples == 0) return;
        std::printf("%-18s samples=%-8llu p50=%-7llu p95=%-7llu p99=%-7llu p99.9=%-8llu max=%llu (ns)\n",
                    label,
                    static_cast<unsigned long long>(r.samples),
                    static_cast<unsigned long long>(r.p50_ns),
                    static_cast<unsigned long long>(r.p95_ns),
                    static_cast<unsigned long long>(r.p99_ns),
                    static_cast<unsigned long long>(r.p999_ns),
                    static_cast<unsigned long long>(r.max_ns));
    };
    for (size_t i = 1; i < argentum::core::kTraceStageCount; ++i) {
        print_latency_row(argentum::core::to_string(static_cast<argentum::core::TraceStage>(i)),
                          lat.stage[i]);
    }
    print_latency_row("wire->gate", lat.wire_to_gate);
    print_latency_row("wire->decision", lat.wire_to_decision);

    if (report.trades_loaded > 1 && engine.load_trades_from_journal(kJournalPath)) {
        const backtest::MonteCarloReport mc = engine.run_monte_carlo(1'000, 0xB0075742ULL);
        std::printf("\n--- Monte Carlo bootstrap (%zu resamples, per-metric percentiles) ---\n", mc.resamples);
        print_metrics("p5", mc.p5);
        print_metrics("p50", mc.p50);
        print_metrics("p95", mc.p95);
        std::printf("note: i.i.d. trade bootstrap — robustness bound, not a market-scenario simulation\n");
    }

    // --- walk-forward: out-of-sample folds with FRESH strategy state ---
    backtest::WalkForwardConfig wf{};
    const uint64_t span_ns = 20'000ULL * 250'000'000ULL;
    wf.fold_test_duration_ns = span_ns / 4;  // 4 non-overlapping folds

    const backtest::WalkForwardReport wf_report = engine.run_walk_forward(factory, config, wf);
    std::printf("\n--- walk-forward (%zu folds, fresh strategy instance per fold) ---\n",
                wf_report.folds.size());
    std::printf("%-6s %10s %10s %8s %12s\n", "fold", "candidates", "accepted", "fills", "pnl");
    for (size_t i = 0; i < wf_report.folds.size(); ++i) {
        const auto& fold = wf_report.folds[i];
        std::printf("%-6zu %10llu %10llu %8zu %12.2f\n",
                    i,
                    static_cast<unsigned long long>(fold.report.funnel.candidates),
                    static_cast<unsigned long long>(fold.report.funnel.gate_accepted),
                    fold.report.trades_loaded,
                    fold.report.metrics.total_pnl);
    }
    std::printf("aggregate OOS (fills=%zu):\n", wf_report.aggregate_trades);
    print_metrics("oos", wf_report.aggregate_out_of_sample);

    // --- cost sensitivity: at which cost multiple does the edge die? ---
    // Governance is deliberately disarmed inside the sweep: a sensitivity
    // analysis must vary ONE factor. With lifecycle armed, different cost
    // levels change which candidates trade, which changes when quarantine
    // hits, and the sweep stops measuring costs.
    backtest::StrategyBacktestConfig sweep_config = config;
    sweep_config.registry = strategy::StrategyRegistryConfig{};
    sweep_config.registry.max_strategy_drawdown_notional = 0.0;
    sweep_config.registry.cusum_lambda_bps = 1e9;
    sweep_config.registration.baseline_sharpe = 0.0;
    const backtest::CostSensitivityReport cost_report =
        engine.run_cost_sensitivity(factory, sweep_config, {0.5, 1.0, 2.0, 3.0, 5.0});
    std::printf("\n--- cost sensitivity (governance disarmed to isolate the cost effect; ---\n");
    std::printf("---                   extreme multipliers double as the cost-shock stress) ---\n");
    std::printf("%-6s %10s %10s %8s %12s\n", "mult", "candidates", "accepted", "fills", "pnl");
    double edge_died_at = 0.0;
    for (const auto& entry : cost_report.entries) {
        std::printf("x%-5.2f %10llu %10llu %8zu %12.2f\n",
                    entry.cost_multiplier,
                    static_cast<unsigned long long>(entry.report.funnel.candidates),
                    static_cast<unsigned long long>(entry.report.funnel.gate_accepted),
                    entry.report.trades_loaded,
                    entry.report.metrics.total_pnl);
        if (edge_died_at == 0.0 && entry.report.funnel.gate_accepted == 0) {
            edge_died_at = entry.cost_multiplier;
        }
    }
    if (edge_died_at > 0.0) {
        std::printf("edge stops surviving the EV gate at x%.2f costs\n", edge_died_at);
    }

    std::printf("\naudit trail: audit.log (signal_decision + strategy_transition events) | journal: %s\n",
                kJournalPath);
    return 0;
}
