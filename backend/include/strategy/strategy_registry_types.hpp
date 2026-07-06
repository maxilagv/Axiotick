#pragma once

#include "ev/ev_types.hpp"
#include "regime/regime_types.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

namespace argentum::strategy {

/**
 * @brief Thresholds for the statistical lifecycle governance.
 *
 * The defaults are documented engineering conventions (same spirit as
 * ADX=25 in Block 2): starting points to recalibrate against each strategy's
 * own backtest volatility (e.g. cusum_lambda_bps ~ 2-3x the backtest stddev
 * of realized_bps), not closed statistical truths.
 */
struct StrategyRegistryConfig {
    // Page-Hinkley (downward mean-shift detector) on realized_bps.
    double cusum_delta_bps = 0.5;    // drift tolerance absorbed before accumulating
    double cusum_lambda_bps = 5.0;   // alarm threshold on PH = M - m

    // Rolling Sharpe floor (raw bps ratio over the window, not annualized).
    size_t sharpe_window = 30;
    double sharpe_floor_ratio = 0.5;  // alarm if ratio < floor_ratio * baseline_sharpe

    // EV decay: one-sided t-stat of (realized - predicted) over the window.
    size_t ev_decay_window = 50;
    size_t ev_decay_min_samples = 20;
    double ev_decay_t_threshold = 2.0;  // alarm if t_stat < -threshold

    // Strategy-level drawdown on the cumulative notional PnL of resolved
    // evaluations (peak-to-trough, quote currency). 0 disables.
    double max_strategy_drawdown_notional = 0.0;

    // UnderObservation -> Active recovery.
    size_t recovery_clean_evaluations = 20;

    // Quarantined -> Disabled escalation (the only automatic path to Disabled).
    size_t max_quarantines_in_lookback = 3;
    uint64_t quarantine_lookback_ns = 90ULL * 86'400ULL * 1'000'000'000ULL;

    // Hard cap per strategy on stored resolved evaluations (FIFO eviction) so
    // long-running processes cannot grow without bound.
    size_t max_resolved_history = 100'000;
};

struct StrategyRegistration {
    // Sharpe-like ratio evidenced by the strategy's own backtest, in the same
    // raw-bps-window units the registry computes. 0 = no baseline yet, which
    // deliberately disables the Sharpe floor (there is no "degradation"
    // without something to degrade from).
    double baseline_sharpe = 0.0;
};

/**
 * @brief One signal evaluated against its own declared horizon.
 *
 * realized_bps compares the market price at entry + expected_holding_hours
 * against the entry price, signed by side — exactly what the signal's EV
 * promised, independent of what other signals did to the shared position.
 */
struct ResolvedEvaluation {
    uint64_t signal_id = 0;
    std::string symbol;
    double predicted_ev_net_bps = 0.0;
    double realized_bps = 0.0;
    double notional = 0.0;
    regime::RegimeLabel regime = regime::RegimeLabel::Unknown;  // regime at signal time
    uint64_t entry_ts_ns = 0;
    uint64_t resolved_ts_ns = 0;
};

struct TransitionEvent {
    ev::StrategyState from = ev::StrategyState::Active;
    ev::StrategyState to = ev::StrategyState::Active;
    std::string reason;
    uint64_t timestamp_ns = 0;
};

/**
 * @brief Operator-grade view of one strategy's statistical health.
 */
struct StrategyHealthSnapshot {
    ev::StrategyState state = ev::StrategyState::Active;
    uint64_t resolved_count = 0;
    uint64_t pending_count = 0;
    double cusum_ph = 0.0;
    double rolling_sharpe_ratio = 0.0;  // 0 until the window is full
    bool sharpe_window_full = false;
    double ev_decay_t_stat = 0.0;       // 0 until min samples reached
    size_t ev_decay_samples = 0;
    double cum_pnl = 0.0;
    double peak_pnl = 0.0;
    double current_drawdown = 0.0;
    size_t clean_evaluations = 0;
    size_t quarantines_in_lookback = 0;
};

} // namespace argentum::strategy
