#pragma once

#include "ev/ev_types.hpp"
#include "regime/regime_types.hpp"
#include "strategy/strategy_registry_types.hpp"

#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace argentum::strategy {

/**
 * @brief Statistical lifecycle governance: the authority that decides
 * active / under_observation / quarantined / disabled per strategy.
 *
 * Attribution model — evaluation-by-horizon, NOT position tracking: each
 * accepted signal is scored against its own declared holding horizon
 * (EVInputs.costs.expected_holding_hours). When the horizon elapses, the
 * market price at that moment is compared with the entry price. This answers
 * the governance question ("is the EV model still valid?") without the
 * FIFO/LIFO lot-accounting that shared positions across signals would
 * otherwise require. See ADR 0013.
 *
 * Transition machine:
 *   Active -> UnderObservation:      Page-Hinkley alarm OR rolling-Sharpe floor breach
 *   UnderObservation -> Active:      recovery_clean_evaluations consecutive clean results
 *   UnderObservation -> Quarantined: strategy drawdown breach OR EV-decay alarm
 *   Active -> Quarantined:           strategy drawdown breach (capital-protective,
 *                                    does not wait for statistical confirmation)
 *   Quarantined -> Active:           manual only (force_state)
 *   Quarantined -> Disabled:         automatic when max_quarantines_in_lookback is
 *                                    reached (the only automatic path to Disabled)
 *
 * Every transition — automatic or manual — is audited as a
 * "strategy_transition" structured event and kept in transition_history().
 * The class takes all timestamps as inputs (no internal clock reads), so
 * backtests are fully deterministic.
 *
 * Not a hot path (tick/bar cadence): one mutex over all state.
 */
class StrategyRegistry {
public:
    explicit StrategyRegistry(StrategyRegistryConfig config);

    /// Optional explicit registration (sets the Sharpe baseline). Strategies
    /// are auto-registered with defaults on first contact otherwise.
    void register_strategy(const std::string& strategy_id, StrategyRegistration info);

    /// Records one accepted fill of a signal for later horizon evaluation.
    void record_signal_open(
        uint64_t signal_id,
        const std::string& strategy_id,
        const std::string& symbol,
        uint8_t side,
        double entry_price,
        double notional,
        double predicted_ev_net_bps,
        double expected_holding_hours,
        regime::RegimeLabel regime,
        uint64_t entry_ts_ns);

    /// Resolves every pending evaluation of `symbol` whose horizon has
    /// elapsed at `timestamp_ns`, then re-runs the governance tests.
    void on_market_tick(const std::string& symbol, double price, uint64_t timestamp_ns);

    /// Resolves ALL remaining pending evaluations of `symbol` regardless of
    /// horizon — called once at the end of a backtest so signals whose
    /// horizon falls beyond the last tick are not silently dropped.
    void flush_pending(const std::string& symbol, double price, uint64_t timestamp_ns);

    [[nodiscard]] ev::StrategyState state(const std::string& strategy_id) const;

    /// Manual override (operator action). Audited. The only way out of
    /// Quarantined/Disabled.
    void force_state(const std::string& strategy_id, ev::StrategyState state, const std::string& reason);

    [[nodiscard]] StrategyHealthSnapshot health(const std::string& strategy_id) const;
    [[nodiscard]] std::vector<ResolvedEvaluation> resolved_history(const std::string& strategy_id) const;
    [[nodiscard]] std::vector<TransitionEvent> transition_history(const std::string& strategy_id) const;
    [[nodiscard]] size_t pending_count() const;

    [[nodiscard]] const StrategyRegistryConfig& config() const { return config_; }

private:
    struct PendingEvaluation {
        uint64_t signal_id = 0;
        std::string strategy_id;
        std::string symbol;
        uint8_t side = 0;
        double entry_price = 0.0;
        double notional = 0.0;
        double predicted_ev_net_bps = 0.0;
        regime::RegimeLabel regime = regime::RegimeLabel::Unknown;
        uint64_t entry_ts_ns = 0;
    };

    struct StrategyBlock {
        StrategyRegistration registration{};
        ev::StrategyState state = ev::StrategyState::Active;

        // Page-Hinkley (downward-shift form): m += (x - mean + delta);
        // M = max(M, m); PH = M - m; alarm when PH > lambda.
        uint64_t ph_count = 0;
        double ph_mean = 0.0;
        double ph_m = 0.0;
        double ph_max_m = 0.0;

        std::deque<double> realized_window;  // sharpe_window
        std::deque<double> diff_window;      // ev_decay_window: realized - predicted

        double cum_pnl = 0.0;
        double peak_pnl = 0.0;

        size_t clean_evaluations = 0;
        std::deque<uint64_t> quarantine_timestamps;

        std::vector<ResolvedEvaluation> resolved;
        std::vector<TransitionEvent> transitions;
    };

    struct Alarms {
        bool cusum = false;
        bool sharpe = false;
        bool ev_decay = false;
        bool drawdown = false;
    };

    // All private helpers require mutex_ held.
    StrategyBlock& block_for(const std::string& strategy_id);
    void resolve_locked(const PendingEvaluation& pending, double price, uint64_t timestamp_ns);
    void apply_governance_locked(const std::string& strategy_id, StrategyBlock& block, uint64_t timestamp_ns);
    [[nodiscard]] Alarms compute_alarms_locked(const StrategyBlock& block) const;
    [[nodiscard]] double rolling_sharpe_locked(const StrategyBlock& block) const;
    [[nodiscard]] double ev_decay_t_locked(const StrategyBlock& block) const;
    [[nodiscard]] size_t quarantines_in_lookback_locked(const StrategyBlock& block, uint64_t now_ns) const;
    void transition_locked(
        const std::string& strategy_id,
        StrategyBlock& block,
        ev::StrategyState to,
        const std::string& reason,
        uint64_t timestamp_ns);

    StrategyRegistryConfig config_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, StrategyBlock> strategies_;
    std::multimap<uint64_t, PendingEvaluation> pending_by_resolve_ns_;
};

} // namespace argentum::strategy
