#pragma once

#include "ev/ev_types.hpp"
#include "regime/regime_types.hpp"

#include <array>
#include <cstdint>

namespace argentum::ev {

/**
 * @brief Configuration for the EV acceptance gate.
 *
 * min_ev_bps_threshold is intentionally a floor above zero: a marginally
 * positive EV inside the estimation noise of p_win is not a tradeable edge.
 * Per-regime multipliers scale that floor (e.g. demand 2x edge under Stress),
 * and allowed_regimes_mask blocks regimes a strategy is not validated for.
 */
struct EVGateConfig {
    double min_ev_bps_threshold = 3.0;
    double min_confidence = 0.55;
    double observation_size_haircut = 0.25;
    std::array<double, regime::kRegimeCount> regime_threshold_multiplier{1.0, 1.0, 1.0, 1.0, 1.0};
    uint32_t allowed_regimes_mask = regime::kAllRegimesMask;
};

/**
 * @brief Stateless net-expected-value gate.
 *
 * ev_gross_bps = p_win * avg_win_bps + (1 - p_win) * avg_loss_bps
 * cost_bps     = 2*taker_fee + 2*half_spread + slippage + impact
 *              + funding_per_hour * holding_hours
 * ev_net_bps   = ev_gross_bps - cost_bps
 *
 * The gate is fail-closed: malformed inputs (NaN, out-of-range probabilities,
 * negative costs, non-positive notional) reject with InvalidInputs instead of
 * defaulting to permissive values. It runs upstream of RiskManager, which
 * remains the final capital defense and is never relaxed by an EV acceptance.
 */
class EVGate {
public:
    explicit EVGate(EVGateConfig config);

    [[nodiscard]] EVResult evaluate(const EVInputs& inputs,
                                    regime::RegimeLabel regime,
                                    StrategyState lifecycle_state) const;

    [[nodiscard]] static double gross_ev_bps(const EVInputs& inputs);
    [[nodiscard]] static double total_cost_bps(const CostModel& costs);
    [[nodiscard]] static bool inputs_valid(const EVInputs& inputs);

    [[nodiscard]] const EVGateConfig& config() const { return config_; }

private:
    EVGateConfig config_;
};

} // namespace argentum::ev
