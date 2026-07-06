#pragma once

#include "core/types.h"
#include "regime/regime_types.hpp"
#include "signal/signal_types.hpp"

#include <optional>
#include <string>

namespace argentum::signal {

/**
 * @brief Contract for strategies that produce EV-gated candidates.
 *
 * Formalizes the shape the Block 1 demo used ad hoc, so the exact same
 * strategy object runs in the live/demo pipeline and inside
 * BacktestEngine::run_strategy — no logic drift between backtest and live.
 *
 * The runner (demo loop or backtest engine) owns regime classification and
 * stamps regime/regime_confidence onto every candidate before gating;
 * strategies may additionally react to regime transitions via
 * on_regime_change (called only when the label actually changes).
 */
class CandidateStrategy {
public:
    virtual ~CandidateStrategy() = default;

    virtual std::optional<SignalCandidate> on_tick(const MarketTick& tick) = 0;

    virtual void on_regime_change(regime::RegimeLabel label, double confidence) {
        (void)label;
        (void)confidence;
    }

    [[nodiscard]] virtual std::string strategy_id() const = 0;
};

} // namespace argentum::signal
