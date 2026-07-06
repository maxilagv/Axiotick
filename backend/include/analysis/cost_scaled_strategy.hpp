#pragma once

#include "signal/candidate_strategy.hpp"

#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace argentum::analysis {

/**
 * @brief Pure decorator for cost-sensitivity sweeps: delegates to an inner
 * strategy and scales every bps cost component of each candidate by a fixed
 * multiplier. expected_holding_hours is NOT scaled (it is time, not cost;
 * scaling funding_bps_per_hour already scales the funding leg).
 *
 * The strategy_id is suffixed with the multiplier so each sweep point gets
 * its own lifecycle ledger in the registry.
 */
class CostScaledStrategy : public signal::CandidateStrategy {
public:
    CostScaledStrategy(std::shared_ptr<signal::CandidateStrategy> inner, double cost_multiplier)
        : inner_(std::move(inner)), cost_multiplier_(cost_multiplier) {
        char suffix[32];
        std::snprintf(suffix, sizeof(suffix), "_cost_x%.2f", cost_multiplier_);
        strategy_id_ = inner_->strategy_id() + suffix;
    }

    std::optional<signal::SignalCandidate> on_tick(const MarketTick& tick) override {
        auto candidate = inner_->on_tick(tick);
        if (!candidate) {
            return candidate;
        }

        candidate->strategy_id = strategy_id_;
        ev::CostModel& costs = candidate->ev_inputs.costs;
        costs.taker_fee_bps *= cost_multiplier_;
        costs.maker_fee_bps *= cost_multiplier_;
        costs.half_spread_bps *= cost_multiplier_;
        costs.slippage_bps *= cost_multiplier_;
        costs.market_impact_bps *= cost_multiplier_;
        costs.funding_bps_per_hour *= cost_multiplier_;
        return candidate;
    }

    void on_regime_change(regime::RegimeLabel label, double confidence) override {
        inner_->on_regime_change(label, confidence);
    }

    [[nodiscard]] std::string strategy_id() const override { return strategy_id_; }

private:
    std::shared_ptr<signal::CandidateStrategy> inner_;
    double cost_multiplier_;
    std::string strategy_id_;
};

} // namespace argentum::analysis
