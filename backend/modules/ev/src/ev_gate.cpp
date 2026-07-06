#include "ev/ev_gate.hpp"

#include <cmath>

namespace argentum::ev {

namespace {

bool finite_non_negative(double value) {
    return std::isfinite(value) && value >= 0.0;
}

bool unit_interval(double value) {
    return std::isfinite(value) && value >= 0.0 && value <= 1.0;
}

} // namespace

const char* to_string(RejectReason reason) {
    switch (reason) {
        case RejectReason::None: return "none";
        case RejectReason::InvalidInputs: return "invalid_inputs";
        case RejectReason::LifecycleBlocked: return "lifecycle_blocked";
        case RejectReason::RegimeBlocked: return "regime_blocked";
        case RejectReason::ConfidenceBelowMin: return "confidence_below_min";
        case RejectReason::EvBelowThreshold: return "ev_below_threshold";
        case RejectReason::MlBridgeUnavailable: return "ml_bridge_unavailable";
        default: return "invalid";
    }
}

const char* to_string(StrategyState state) {
    switch (state) {
        case StrategyState::Active: return "active";
        case StrategyState::UnderObservation: return "under_observation";
        case StrategyState::Quarantined: return "quarantined";
        case StrategyState::Disabled: return "disabled";
        default: return "invalid";
    }
}

EVGate::EVGate(EVGateConfig config) : config_(config) {}

double EVGate::gross_ev_bps(const EVInputs& inputs) {
    return inputs.p_win * inputs.avg_win_bps + (1.0 - inputs.p_win) * inputs.avg_loss_bps;
}

double EVGate::total_cost_bps(const CostModel& costs) {
    return 2.0 * costs.taker_fee_bps
         + 2.0 * costs.half_spread_bps
         + costs.slippage_bps
         + costs.market_impact_bps
         + costs.funding_bps_per_hour * costs.expected_holding_hours;
}

bool EVGate::inputs_valid(const EVInputs& inputs) {
    if (!unit_interval(inputs.p_win)) return false;
    if (!unit_interval(inputs.confidence)) return false;
    if (!std::isfinite(inputs.avg_win_bps) || inputs.avg_win_bps < 0.0) return false;
    if (!std::isfinite(inputs.avg_loss_bps) || inputs.avg_loss_bps > 0.0) return false;
    if (!std::isfinite(inputs.notional) || inputs.notional <= 0.0) return false;

    const CostModel& c = inputs.costs;
    return finite_non_negative(c.taker_fee_bps)
        && finite_non_negative(c.maker_fee_bps)
        && finite_non_negative(c.half_spread_bps)
        && finite_non_negative(c.slippage_bps)
        && finite_non_negative(c.funding_bps_per_hour)
        && finite_non_negative(c.expected_holding_hours)
        && finite_non_negative(c.market_impact_bps);
}

EVResult EVGate::evaluate(const EVInputs& inputs,
                          regime::RegimeLabel regime,
                          StrategyState lifecycle_state) const {
    EVResult result{};

    if (!inputs_valid(inputs) || !regime::is_valid_regime(regime)) {
        result.reject_reason = RejectReason::InvalidInputs;
        return result;
    }

    result.ev_gross_bps = gross_ev_bps(inputs);
    result.cost_bps = total_cost_bps(inputs.costs);
    result.ev_net_bps = result.ev_gross_bps - result.cost_bps;
    result.ev_net_notional = (result.ev_net_bps / 10'000.0) * inputs.notional;
    result.effective_threshold_bps =
        config_.min_ev_bps_threshold *
        config_.regime_threshold_multiplier[static_cast<size_t>(regime)];

    if (lifecycle_state == StrategyState::Quarantined ||
        lifecycle_state == StrategyState::Disabled) {
        result.reject_reason = RejectReason::LifecycleBlocked;
        return result;
    }

    if ((config_.allowed_regimes_mask & regime::regime_bit(regime)) == 0) {
        result.reject_reason = RejectReason::RegimeBlocked;
        return result;
    }

    if (inputs.confidence < config_.min_confidence) {
        result.reject_reason = RejectReason::ConfidenceBelowMin;
        return result;
    }

    if (result.ev_net_bps < result.effective_threshold_bps) {
        result.reject_reason = RejectReason::EvBelowThreshold;
        return result;
    }

    result.accepted = true;
    result.size_multiplier = (lifecycle_state == StrategyState::UnderObservation)
        ? config_.observation_size_haircut
        : 1.0;
    return result;
}

} // namespace argentum::ev
