#include "strategy/strategy_registry.hpp"

#include "audit/logger.hpp"
#include "core/types.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace argentum::strategy {

namespace {

std::string format_double(double value) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.6f", value);
    return std::string(buffer);
}

std::string format_u64(uint64_t value) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%llu", static_cast<unsigned long long>(value));
    return std::string(buffer);
}

double window_mean(const std::deque<double>& window) {
    if (window.empty()) return 0.0;
    double sum = 0.0;
    for (double v : window) sum += v;
    return sum / static_cast<double>(window.size());
}

} // namespace

StrategyRegistry::StrategyRegistry(StrategyRegistryConfig config) : config_(config) {}

StrategyRegistry::StrategyBlock& StrategyRegistry::block_for(const std::string& strategy_id) {
    return strategies_[strategy_id];  // auto-registers with defaults on first contact
}

void StrategyRegistry::register_strategy(const std::string& strategy_id, StrategyRegistration info) {
    std::lock_guard<std::mutex> lock(mutex_);
    block_for(strategy_id).registration = info;
}

void StrategyRegistry::record_signal_open(
    uint64_t signal_id,
    const std::string& strategy_id,
    const std::string& symbol,
    uint8_t side,
    double entry_price,
    double notional,
    double predicted_ev_net_bps,
    double expected_holding_hours,
    regime::RegimeLabel regime,
    uint64_t entry_ts_ns) {
    if (!(entry_price > 0.0) || !std::isfinite(entry_price) || strategy_id.empty()) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    (void)block_for(strategy_id);

    PendingEvaluation pending{};
    pending.signal_id = signal_id;
    pending.strategy_id = strategy_id;
    pending.symbol = symbol;
    pending.side = side;
    pending.entry_price = entry_price;
    pending.notional = notional;
    pending.predicted_ev_net_bps = predicted_ev_net_bps;
    pending.regime = regime;
    pending.entry_ts_ns = entry_ts_ns;

    const double hours = (std::isfinite(expected_holding_hours) && expected_holding_hours > 0.0)
        ? expected_holding_hours
        : 0.0;  // 0-horizon evaluations resolve on the symbol's next tick
    const uint64_t horizon_ns = static_cast<uint64_t>(hours * 3'600.0 * 1e9);
    pending_by_resolve_ns_.emplace(entry_ts_ns + horizon_ns, std::move(pending));
}

void StrategyRegistry::on_market_tick(const std::string& symbol, double price, uint64_t timestamp_ns) {
    if (!(price > 0.0) || !std::isfinite(price)) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    auto it = pending_by_resolve_ns_.begin();
    while (it != pending_by_resolve_ns_.end() && it->first <= timestamp_ns) {
        if (it->second.symbol == symbol) {
            const PendingEvaluation pending = std::move(it->second);
            it = pending_by_resolve_ns_.erase(it);
            resolve_locked(pending, price, timestamp_ns);
        } else {
            // Due, but belongs to another symbol: leave it for that symbol's
            // own next tick instead of resolving against the wrong price.
            ++it;
        }
    }
}

void StrategyRegistry::flush_pending(const std::string& symbol, double price, uint64_t timestamp_ns) {
    if (!(price > 0.0) || !std::isfinite(price)) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    auto it = pending_by_resolve_ns_.begin();
    while (it != pending_by_resolve_ns_.end()) {
        if (it->second.symbol == symbol) {
            const PendingEvaluation pending = std::move(it->second);
            it = pending_by_resolve_ns_.erase(it);
            resolve_locked(pending, price, timestamp_ns);
        } else {
            ++it;
        }
    }
}

void StrategyRegistry::resolve_locked(
    const PendingEvaluation& pending,
    double price,
    uint64_t timestamp_ns) {
    StrategyBlock& block = block_for(pending.strategy_id);

    ResolvedEvaluation resolved{};
    resolved.signal_id = pending.signal_id;
    resolved.symbol = pending.symbol;
    resolved.predicted_ev_net_bps = pending.predicted_ev_net_bps;
    resolved.notional = pending.notional;
    resolved.regime = pending.regime;
    resolved.entry_ts_ns = pending.entry_ts_ns;
    resolved.resolved_ts_ns = timestamp_ns;

    const double direction = (pending.side == SIDE_BUY) ? 1.0 : -1.0;
    resolved.realized_bps = (price / pending.entry_price - 1.0) * 10'000.0 * direction;

    // Page-Hinkley update (downward-shift form): a sustained drop of the mean
    // beyond `delta` makes m fall while M remembers the peak; PH = M - m.
    const double x = resolved.realized_bps;
    ++block.ph_count;
    block.ph_mean += (x - block.ph_mean) / static_cast<double>(block.ph_count);
    block.ph_m += (x - block.ph_mean + config_.cusum_delta_bps);
    block.ph_max_m = std::max(block.ph_max_m, block.ph_m);

    block.realized_window.push_back(x);
    if (block.realized_window.size() > config_.sharpe_window) {
        block.realized_window.pop_front();
    }

    block.diff_window.push_back(resolved.realized_bps - resolved.predicted_ev_net_bps);
    if (block.diff_window.size() > config_.ev_decay_window) {
        block.diff_window.pop_front();
    }

    block.cum_pnl += resolved.notional * resolved.realized_bps / 10'000.0;
    block.peak_pnl = std::max(block.peak_pnl, block.cum_pnl);

    block.resolved.push_back(resolved);
    if (block.resolved.size() > config_.max_resolved_history) {
        block.resolved.erase(block.resolved.begin());
    }

    apply_governance_locked(pending.strategy_id, block, timestamp_ns);
}

double StrategyRegistry::rolling_sharpe_locked(const StrategyBlock& block) const {
    const auto& window = block.realized_window;
    if (window.size() < config_.sharpe_window || window.size() < 2) {
        return 0.0;
    }

    const double mean = window_mean(window);
    double variance = 0.0;
    for (double v : window) {
        const double d = v - mean;
        variance += d * d;
    }
    variance /= static_cast<double>(window.size());
    const double stddev = std::sqrt(variance);
    if (stddev < 1e-9) {
        return (mean >= 0.0) ? 1e9 : -1e9;
    }
    return mean / stddev;
}

double StrategyRegistry::ev_decay_t_locked(const StrategyBlock& block) const {
    const auto& window = block.diff_window;
    const size_t n = window.size();
    if (n < config_.ev_decay_min_samples || n < 2) {
        return 0.0;
    }

    const double mean = window_mean(window);
    double variance = 0.0;
    for (double v : window) {
        const double d = v - mean;
        variance += d * d;
    }
    variance /= static_cast<double>(n - 1);  // sample stddev, standard t-test convention
    const double denom = std::max(std::sqrt(variance) / std::sqrt(static_cast<double>(n)), 1e-12);
    return mean / denom;
}

size_t StrategyRegistry::quarantines_in_lookback_locked(
    const StrategyBlock& block,
    uint64_t now_ns) const {
    size_t count = 0;
    for (uint64_t ts : block.quarantine_timestamps) {
        if (now_ns <= ts || (now_ns - ts) <= config_.quarantine_lookback_ns) {
            ++count;
        }
    }
    return count;
}

StrategyRegistry::Alarms StrategyRegistry::compute_alarms_locked(const StrategyBlock& block) const {
    Alarms alarms{};

    alarms.cusum = (block.ph_max_m - block.ph_m) > config_.cusum_lambda_bps;

    if (block.registration.baseline_sharpe > 0.0 &&
        block.realized_window.size() >= config_.sharpe_window) {
        alarms.sharpe = rolling_sharpe_locked(block) <
                        config_.sharpe_floor_ratio * block.registration.baseline_sharpe;
    }

    if (block.diff_window.size() >= config_.ev_decay_min_samples) {
        alarms.ev_decay = ev_decay_t_locked(block) < -config_.ev_decay_t_threshold;
    }

    if (config_.max_strategy_drawdown_notional > 0.0) {
        alarms.drawdown = (block.peak_pnl - block.cum_pnl) > config_.max_strategy_drawdown_notional;
    }

    return alarms;
}

void StrategyRegistry::apply_governance_locked(
    const std::string& strategy_id,
    StrategyBlock& block,
    uint64_t timestamp_ns) {
    const Alarms alarms = compute_alarms_locked(block);

    switch (block.state) {
        case ev::StrategyState::Active: {
            if (alarms.drawdown) {
                transition_locked(strategy_id, block, ev::StrategyState::Quarantined,
                                  "strategy_drawdown_breach", timestamp_ns);
            } else if (alarms.cusum || alarms.sharpe) {
                transition_locked(strategy_id, block, ev::StrategyState::UnderObservation,
                                  alarms.cusum ? "cusum_alarm" : "rolling_sharpe_floor",
                                  timestamp_ns);
            }
            break;
        }
        case ev::StrategyState::UnderObservation: {
            if (alarms.drawdown || alarms.ev_decay) {
                transition_locked(strategy_id, block, ev::StrategyState::Quarantined,
                                  alarms.drawdown ? "strategy_drawdown_breach" : "ev_decay",
                                  timestamp_ns);
            } else if (alarms.cusum || alarms.sharpe) {
                block.clean_evaluations = 0;
            } else {
                ++block.clean_evaluations;
                if (block.clean_evaluations >= config_.recovery_clean_evaluations) {
                    transition_locked(strategy_id, block, ev::StrategyState::Active,
                                      "recovery", timestamp_ns);
                }
            }
            break;
        }
        case ev::StrategyState::Quarantined:
        case ev::StrategyState::Disabled:
            // No automatic exits. Evaluations of fills accepted before the
            // block keep resolving for forensics, but do not move the state.
            break;
    }
}

void StrategyRegistry::transition_locked(
    const std::string& strategy_id,
    StrategyBlock& block,
    ev::StrategyState to,
    const std::string& reason,
    uint64_t timestamp_ns) {
    const ev::StrategyState from = block.state;
    if (from == to) {
        return;
    }

    block.state = to;
    block.transitions.push_back(TransitionEvent{from, to, reason, timestamp_ns});

    // Fresh detector for the next break; the rolling windows are kept on
    // purpose — recovery must be earned by the window actually healing.
    block.ph_count = 0;
    block.ph_mean = 0.0;
    block.ph_m = 0.0;
    block.ph_max_m = 0.0;
    block.clean_evaluations = 0;

    audit::Logger::instance().structured_log(
        audit::LogLevel::AUDIT,
        "strategy_transition",
        {{"strategy_id", strategy_id},
         {"from", ev::to_string(from)},
         {"to", ev::to_string(to)},
         {"reason", reason},
         {"event_ts_ns", format_u64(timestamp_ns)},
         {"cum_pnl", format_double(block.cum_pnl)},
         {"peak_pnl", format_double(block.peak_pnl)}});

    if (to == ev::StrategyState::Quarantined) {
        block.quarantine_timestamps.push_back(timestamp_ns);
        while (!block.quarantine_timestamps.empty() &&
               timestamp_ns > block.quarantine_timestamps.front() &&
               (timestamp_ns - block.quarantine_timestamps.front()) > config_.quarantine_lookback_ns) {
            block.quarantine_timestamps.pop_front();
        }

        if (block.quarantine_timestamps.size() >= config_.max_quarantines_in_lookback) {
            transition_locked(strategy_id, block, ev::StrategyState::Disabled,
                              "repeated_quarantines", timestamp_ns);
        }
    }
}

ev::StrategyState StrategyRegistry::state(const std::string& strategy_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = strategies_.find(strategy_id);
    return (it != strategies_.end()) ? it->second.state : ev::StrategyState::Active;
}

void StrategyRegistry::force_state(
    const std::string& strategy_id,
    ev::StrategyState state,
    const std::string& reason) {
    std::lock_guard<std::mutex> lock(mutex_);
    StrategyBlock& block = block_for(strategy_id);
    // Manual overrides bypass the machine's automatic rules by design; the
    // quarantine counter still applies if an operator quarantines manually.
    transition_locked(strategy_id, block, state, "manual:" + reason, 0);
}

StrategyHealthSnapshot StrategyRegistry::health(const std::string& strategy_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    StrategyHealthSnapshot snapshot{};

    const auto it = strategies_.find(strategy_id);
    if (it == strategies_.end()) {
        return snapshot;
    }
    const StrategyBlock& block = it->second;

    snapshot.state = block.state;
    snapshot.resolved_count = block.resolved.size();
    snapshot.cusum_ph = block.ph_max_m - block.ph_m;
    snapshot.sharpe_window_full = block.realized_window.size() >= config_.sharpe_window;
    snapshot.rolling_sharpe_ratio = snapshot.sharpe_window_full ? rolling_sharpe_locked(block) : 0.0;
    snapshot.ev_decay_samples = block.diff_window.size();
    snapshot.ev_decay_t_stat =
        (block.diff_window.size() >= config_.ev_decay_min_samples) ? ev_decay_t_locked(block) : 0.0;
    snapshot.cum_pnl = block.cum_pnl;
    snapshot.peak_pnl = block.peak_pnl;
    snapshot.current_drawdown = block.peak_pnl - block.cum_pnl;
    snapshot.clean_evaluations = block.clean_evaluations;
    snapshot.quarantines_in_lookback = block.quarantine_timestamps.size();

    for (const auto& [resolve_ns, pending] : pending_by_resolve_ns_) {
        (void)resolve_ns;
        if (pending.strategy_id == strategy_id) {
            ++snapshot.pending_count;
        }
    }
    return snapshot;
}

std::vector<ResolvedEvaluation> StrategyRegistry::resolved_history(const std::string& strategy_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = strategies_.find(strategy_id);
    return (it != strategies_.end()) ? it->second.resolved : std::vector<ResolvedEvaluation>{};
}

std::vector<TransitionEvent> StrategyRegistry::transition_history(const std::string& strategy_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = strategies_.find(strategy_id);
    return (it != strategies_.end()) ? it->second.transitions : std::vector<TransitionEvent>{};
}

size_t StrategyRegistry::pending_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return pending_by_resolve_ns_.size();
}

} // namespace argentum::strategy
