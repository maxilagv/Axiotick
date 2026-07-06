#pragma once

#include <cstddef>
#include <cstdint>

namespace argentum::ev {

/**
 * @brief Modeled round-trip trading costs, all expressed in basis points of notional.
 *
 * Entry and exit legs are charged for fees and spread (hence the 2x factors in
 * the gate math). Funding applies to derivatives held over time and must be 0
 * for spot/equities. Every component must be finite and non-negative.
 */
struct CostModel {
    double taker_fee_bps = 0.0;
    double maker_fee_bps = 0.0;
    double half_spread_bps = 0.0;        // top-of-book half spread at signal time
    double slippage_bps = 0.0;           // size-vs-depth model output
    double funding_bps_per_hour = 0.0;   // perpetual funding drag; 0 outside perps
    double expected_holding_hours = 0.0;
    double market_impact_bps = 0.0;      // ~ k * sqrt(size / ADV)
};

/**
 * @brief Inputs required to price the expected value of one candidate trade.
 *
 * avg_win_bps must be >= 0 and avg_loss_bps must be <= 0; both are expected
 * favorable/adverse moves over the holding horizon, in basis points.
 */
struct EVInputs {
    double p_win = 0.0;        // probability of the favorable outcome, [0, 1]
    double avg_win_bps = 0.0;
    double avg_loss_bps = 0.0;
    double confidence = 0.0;   // model/rule calibration score, [0, 1]
    double notional = 0.0;     // order notional in quote currency, > 0
    CostModel costs{};
};

enum class RejectReason : uint8_t {
    None = 0,
    InvalidInputs = 1,
    LifecycleBlocked = 2,
    RegimeBlocked = 3,
    ConfidenceBelowMin = 4,
    EvBelowThreshold = 5,
    MlBridgeUnavailable = 6   // produced by callers when model serving times out
};

inline constexpr size_t kRejectReasonCount = 7;

[[nodiscard]] const char* to_string(RejectReason reason);

/**
 * @brief Strategy lifecycle states enforced at the EV gate.
 *
 * The authoritative registry with statistical transitions (CUSUM, rolling
 * Sharpe, EV decay) lands in Block 3; the gate consumes the state from day one
 * so enforcement semantics never change.
 */
enum class StrategyState : uint8_t {
    Active = 0,
    UnderObservation = 1,   // accepted with a size haircut
    Quarantined = 2,        // blocked; manual re-enable only
    Disabled = 3            // blocked
};

[[nodiscard]] const char* to_string(StrategyState state);

/**
 * @brief Full EV decision payload. Populated on both accept and reject so the
 * audit trail always records the numbers behind the decision.
 */
struct EVResult {
    double ev_gross_bps = 0.0;
    double cost_bps = 0.0;
    double ev_net_bps = 0.0;
    double ev_net_notional = 0.0;
    double effective_threshold_bps = 0.0;  // min threshold after regime multiplier
    double size_multiplier = 1.0;          // lifecycle haircut; 1.0 = full size
    bool accepted = false;
    RejectReason reject_reason = RejectReason::None;
};

} // namespace argentum::ev
