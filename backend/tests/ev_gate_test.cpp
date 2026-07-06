#include "ev/ev_gate.hpp"

#include <cmath>
#include <cstdio>
#include <limits>

// assert() compiles out under NDEBUG (Release), so use an always-on check.
static int g_failures = 0;
#define CHECK(cond) \
    do { \
        if (!(cond)) { \
            std::printf("CHECK FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures; \
        } \
    } while (0)
#define CHECK_NEAR(a, b, eps) CHECK(std::abs((a) - (b)) <= (eps))

using argentum::ev::CostModel;
using argentum::ev::EVGate;
using argentum::ev::EVGateConfig;
using argentum::ev::EVInputs;
using argentum::ev::EVResult;
using argentum::ev::RejectReason;
using argentum::ev::StrategyState;
using argentum::regime::RegimeLabel;

namespace {

EVInputs baseline_inputs() {
    EVInputs inputs{};
    inputs.p_win = 0.6;
    inputs.avg_win_bps = 20.0;
    inputs.avg_loss_bps = -10.0;
    inputs.confidence = 0.8;
    inputs.notional = 10'000.0;
    inputs.costs.taker_fee_bps = 2.0;
    inputs.costs.half_spread_bps = 0.5;
    inputs.costs.slippage_bps = 0.5;
    inputs.costs.market_impact_bps = 0.3;
    inputs.costs.funding_bps_per_hour = 1.0;
    inputs.costs.expected_holding_hours = 0.5;
    return inputs;
}

void test_ev_math() {
    const EVInputs inputs = baseline_inputs();

    // gross = 0.6*20 + 0.4*(-10) = 8.0
    CHECK_NEAR(EVGate::gross_ev_bps(inputs), 8.0, 1e-9);
    // cost = 2*2 + 2*0.5 + 0.5 + 0.3 + 1.0*0.5 = 6.3
    CHECK_NEAR(EVGate::total_cost_bps(inputs.costs), 6.3, 1e-9);

    // Default threshold 3.0 > net 1.7 -> rejected, but numbers still populated.
    EVGate strict(EVGateConfig{});
    const EVResult rejected = strict.evaluate(inputs, RegimeLabel::Unknown, StrategyState::Active);
    CHECK(!rejected.accepted);
    CHECK(rejected.reject_reason == RejectReason::EvBelowThreshold);
    CHECK_NEAR(rejected.ev_gross_bps, 8.0, 1e-9);
    CHECK_NEAR(rejected.cost_bps, 6.3, 1e-9);
    CHECK_NEAR(rejected.ev_net_bps, 1.7, 1e-9);
    CHECK_NEAR(rejected.ev_net_notional, 1.7, 1e-9);  // 1.7bps of 10k
    CHECK_NEAR(rejected.effective_threshold_bps, 3.0, 1e-9);

    // Lower the floor to 1.0 -> accepted at full size.
    EVGateConfig loose{};
    loose.min_ev_bps_threshold = 1.0;
    EVGate gate(loose);
    const EVResult accepted = gate.evaluate(inputs, RegimeLabel::Unknown, StrategyState::Active);
    CHECK(accepted.accepted);
    CHECK(accepted.reject_reason == RejectReason::None);
    CHECK_NEAR(accepted.size_multiplier, 1.0, 1e-12);
}

void test_confidence_gate() {
    EVGateConfig config{};
    config.min_ev_bps_threshold = 1.0;
    config.min_confidence = 0.55;
    EVGate gate(config);

    EVInputs inputs = baseline_inputs();
    inputs.confidence = 0.5;
    const EVResult result = gate.evaluate(inputs, RegimeLabel::Unknown, StrategyState::Active);
    CHECK(!result.accepted);
    CHECK(result.reject_reason == RejectReason::ConfidenceBelowMin);
}

void test_regime_mask_and_multiplier() {
    // Inputs engineered for ev_net = 5.0:
    // gross = 0.6*20 + 0.4*(-10) = 8; cost = 2*1 + 2*0.25 + 0.2 + 0.1 + 0.4*0.5 = 3.0
    EVInputs inputs = baseline_inputs();
    inputs.costs.taker_fee_bps = 1.0;
    inputs.costs.half_spread_bps = 0.25;
    inputs.costs.slippage_bps = 0.2;
    inputs.costs.market_impact_bps = 0.1;
    inputs.costs.funding_bps_per_hour = 0.4;
    inputs.costs.expected_holding_hours = 0.5;

    EVGateConfig config{};
    config.min_ev_bps_threshold = 3.0;
    config.regime_threshold_multiplier[static_cast<size_t>(RegimeLabel::Stress)] = 2.0;
    EVGate gate(config);

    // Trend: threshold 3.0, net 5.0 -> accepted.
    const EVResult trend = gate.evaluate(inputs, RegimeLabel::Trend, StrategyState::Active);
    CHECK(trend.accepted);
    CHECK_NEAR(trend.ev_net_bps, 5.0, 1e-9);
    CHECK_NEAR(trend.effective_threshold_bps, 3.0, 1e-9);

    // Stress: threshold doubled to 6.0 -> same edge rejected.
    const EVResult stress = gate.evaluate(inputs, RegimeLabel::Stress, StrategyState::Active);
    CHECK(!stress.accepted);
    CHECK(stress.reject_reason == RejectReason::EvBelowThreshold);
    CHECK_NEAR(stress.effective_threshold_bps, 6.0, 1e-9);

    // Mask out Stress entirely -> RegimeBlocked before any threshold math matters.
    EVGateConfig masked{};
    masked.min_ev_bps_threshold = 1.0;
    masked.allowed_regimes_mask =
        argentum::regime::kAllRegimesMask & ~argentum::regime::regime_bit(RegimeLabel::Stress);
    EVGate masked_gate(masked);
    const EVResult blocked = masked_gate.evaluate(inputs, RegimeLabel::Stress, StrategyState::Active);
    CHECK(!blocked.accepted);
    CHECK(blocked.reject_reason == RejectReason::RegimeBlocked);
}

void test_lifecycle_gate() {
    EVGateConfig config{};
    config.min_ev_bps_threshold = 1.0;
    config.observation_size_haircut = 0.25;
    EVGate gate(config);
    const EVInputs inputs = baseline_inputs();

    const EVResult quarantined =
        gate.evaluate(inputs, RegimeLabel::Unknown, StrategyState::Quarantined);
    CHECK(!quarantined.accepted);
    CHECK(quarantined.reject_reason == RejectReason::LifecycleBlocked);

    const EVResult disabled =
        gate.evaluate(inputs, RegimeLabel::Unknown, StrategyState::Disabled);
    CHECK(!disabled.accepted);
    CHECK(disabled.reject_reason == RejectReason::LifecycleBlocked);

    const EVResult observed =
        gate.evaluate(inputs, RegimeLabel::Unknown, StrategyState::UnderObservation);
    CHECK(observed.accepted);
    CHECK_NEAR(observed.size_multiplier, 0.25, 1e-12);
}

void test_invalid_inputs_fail_closed() {
    EVGateConfig config{};
    config.min_ev_bps_threshold = 0.0;
    config.min_confidence = 0.0;
    EVGate gate(config);

    EVInputs bad_p = baseline_inputs();
    bad_p.p_win = 1.2;
    CHECK(gate.evaluate(bad_p, RegimeLabel::Unknown, StrategyState::Active).reject_reason ==
          RejectReason::InvalidInputs);

    EVInputs bad_notional = baseline_inputs();
    bad_notional.notional = 0.0;
    CHECK(gate.evaluate(bad_notional, RegimeLabel::Unknown, StrategyState::Active).reject_reason ==
          RejectReason::InvalidInputs);

    EVInputs bad_loss = baseline_inputs();
    bad_loss.avg_loss_bps = 5.0;  // adverse move must be <= 0
    CHECK(gate.evaluate(bad_loss, RegimeLabel::Unknown, StrategyState::Active).reject_reason ==
          RejectReason::InvalidInputs);

    EVInputs bad_cost = baseline_inputs();
    bad_cost.costs.slippage_bps = std::numeric_limits<double>::quiet_NaN();
    CHECK(gate.evaluate(bad_cost, RegimeLabel::Unknown, StrategyState::Active).reject_reason ==
          RejectReason::InvalidInputs);

    EVInputs bad_conf = baseline_inputs();
    bad_conf.confidence = -0.1;
    CHECK(gate.evaluate(bad_conf, RegimeLabel::Unknown, StrategyState::Active).reject_reason ==
          RejectReason::InvalidInputs);

    const EVInputs good = baseline_inputs();
    CHECK(gate.evaluate(good, static_cast<RegimeLabel>(9), StrategyState::Active).reject_reason ==
          RejectReason::InvalidInputs);
}

} // namespace

int main() {
    test_ev_math();
    test_confidence_gate();
    test_regime_mask_and_multiplier();
    test_lifecycle_gate();
    test_invalid_inputs_fail_closed();

    if (g_failures != 0) {
        std::printf("ev_gate_test FAILED (%d checks)\n", g_failures);
        return 1;
    }
    std::printf("ev_gate_test passed\n");
    return 0;
}
