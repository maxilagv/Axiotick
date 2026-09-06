#pragma once

#include "core/types.h"
#include "ev/ev_types.hpp"
#include "regime/regime_types.hpp"

#include <cstdint>
#include <string>

namespace argentum::signal {

/**
 * @brief A candidate trade produced by a strategy/model, before EV gating.
 *
 * ev_inputs must already carry calibrated probabilities and a cost model for
 * the venue; the gate never invents missing values (fail-closed).
 */
struct SignalCandidate {
    std::string strategy_id;
    std::string model_version;        // e.g. "sma_cross_rule_v1" or "gbm_v3_2026-08-01"
    std::string feature_snapshot_id;  // reference to the persisted feature vector
    std::string symbol;
    uint8_t side = SIDE_BUY;
    uint8_t order_type = ORDER_TYPE_LIMIT;
    uint8_t tif = TIF_GTC;
    double price = 0.0;               // reference/limit price for the candidate order
    double quantity = 0.0;            // pre-haircut quantity
    uint64_t timestamp_ns = 0;        // 0 = stamped by the engine
    uint64_t origin_tick_id = 0;      // trace id of the tick that produced this candidate (0 = unknown)
    ev::EVInputs ev_inputs{};
    regime::RegimeLabel regime = regime::RegimeLabel::Unknown;
    double regime_confidence = 0.0;
};

/**
 * @brief Fully-resolved signal decision: candidate + EV verdict + OMS outcome.
 *
 * This struct mirrors exactly what is written to the audit trail, so any
 * decision can be reconstructed (inputs, model version, probabilities, EV,
 * accept/reject reason, resulting order id).
 */
struct Signal {
    uint64_t signal_id = 0;
    SignalCandidate candidate{};
    ev::StrategyState lifecycle_state = ev::StrategyState::Active;
    ev::EVResult ev_result{};
    double submitted_quantity = 0.0;  // candidate quantity after lifecycle haircut
    uint64_t submitted_order_id = 0;  // 0 when the gate rejected
    bool order_accepted = false;      // OMS/risk verdict for the submitted order
    std::string decision_reason;      // human-readable summary for operators
};

} // namespace argentum::signal
