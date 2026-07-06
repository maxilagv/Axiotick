#pragma once

// Reference/demo strategy shared by argentum_signal_demo and the backtest
// engine's counterfactual mode. Deliberately trivial: SMA(20) vs SMA(50)
// crossover with volatility-scaled p_win / expected-move heuristics. It
// exists to exercise the decision machinery end to end, NOT as an alpha
// claim — and it must stay honestly labeled as such.

#include "analysis/strategy.hpp"
#include "core/types.h"
#include "signal/candidate_strategy.hpp"

#include <algorithm>
#include <cmath>
#include <deque>
#include <optional>
#include <string>

namespace argentum::analysis {

/// Rolling standard deviation of tick-to-tick returns, in bps.
class RollingReturnStd {
public:
    explicit RollingReturnStd(size_t window) : window_(window == 0 ? 1 : window) {}

    void add_price(double price) {
        if (last_price_ > 0.0) {
            const double ret_bps = (price / last_price_ - 1.0) * 10'000.0;
            returns_.push_back(ret_bps);
            sum_ += ret_bps;
            sum_sq_ += ret_bps * ret_bps;
            if (returns_.size() > window_) {
                const double dropped = returns_.front();
                returns_.pop_front();
                sum_ -= dropped;
                sum_sq_ -= dropped * dropped;
            }
        }
        last_price_ = price;
    }

    [[nodiscard]] bool ready() const { return returns_.size() >= window_; }

    [[nodiscard]] double stddev_bps() const {
        if (returns_.size() < 2) return 0.0;
        const double n = static_cast<double>(returns_.size());
        const double mean = sum_ / n;
        const double variance = std::max(0.0, sum_sq_ / n - mean * mean);
        return std::sqrt(variance);
    }

private:
    size_t window_;
    std::deque<double> returns_;
    double sum_ = 0.0;
    double sum_sq_ = 0.0;
    double last_price_ = 0.0;
};

/**
 * @brief SMA(20/50) crossover, confirmed kConfirmationTicks after the cross
 * (at the exact crossing point the SMA gap is ~0 by definition; the delay
 * lets a real trend separate from noise before scoring).
 */
class SmaCrossoverStrategy : public signal::CandidateStrategy {
public:
    static constexpr int kConfirmationTicks = 5;

    // expected_holding_hours must be sized to the tick density of the data:
    // horizon evaluations measure the market move over exactly this window.
    SmaCrossoverStrategy(std::string symbol, double target_notional,
                         double expected_holding_hours = 0.25)
        : symbol_(std::move(symbol)),
          target_notional_(target_notional),
          expected_holding_hours_(expected_holding_hours) {}

    std::optional<signal::SignalCandidate> on_tick(const MarketTick& tick) override {
        fast_.add(tick.price);
        slow_.add(tick.price);
        vol_.add_price(tick.price);

        if (!fast_.is_ready() || !slow_.is_ready() || !vol_.ready()) {
            return std::nullopt;
        }

        const double fast = fast_.value();
        const double slow = slow_.value();
        const int direction = (fast > slow) ? 1 : (fast < slow ? -1 : 0);
        if (direction == 0) {
            return std::nullopt;
        }

        if (direction != last_direction_) {
            // Fresh cross: arm the confirmation countdown instead of firing now.
            last_direction_ = direction;
            pending_direction_ = direction;
            confirmation_countdown_ = kConfirmationTicks;
            return std::nullopt;
        }

        if (pending_direction_ != direction || confirmation_countdown_ < 0) {
            return std::nullopt;
        }
        if (confirmation_countdown_-- > 0) {
            return std::nullopt;
        }
        pending_direction_ = 0;  // fire exactly once per confirmed cross

        // Heuristic demo estimates. Horizon ~ 60 ticks; expected favorable move
        // 2 sigma_h with a 1 sigma_h stop. p_win/confidence scale with how far
        // the SMAs separated relative to noise.
        const double sigma_tick_bps = vol_.stddev_bps();
        const double sigma_horizon_bps = sigma_tick_bps * std::sqrt(60.0);
        const double gap_bps = std::abs(fast / slow - 1.0) * 10'000.0;
        const double gap_over_noise = (sigma_tick_bps > 1e-9) ? gap_bps / sigma_tick_bps : 0.0;

        signal::SignalCandidate candidate{};
        candidate.strategy_id = strategy_id();
        candidate.model_version = "sma_cross_rule_v1";
        candidate.feature_snapshot_id = "demo_tick_" + std::to_string(tick.timestamp_ns);
        candidate.symbol = symbol_;
        candidate.side = static_cast<uint8_t>((direction > 0) ? SIDE_BUY : SIDE_SELL);
        candidate.price = tick.price;
        candidate.quantity = target_notional_ / tick.price;
        candidate.timestamp_ns = tick.timestamp_ns;

        candidate.ev_inputs.p_win = std::min(0.62, 0.50 + 0.025 * gap_over_noise);
        candidate.ev_inputs.avg_win_bps = 2.0 * sigma_horizon_bps;
        candidate.ev_inputs.avg_loss_bps = -sigma_horizon_bps;
        candidate.ev_inputs.confidence = std::min(0.95, 0.50 + 0.06 * gap_over_noise);
        candidate.ev_inputs.notional = target_notional_;

        candidate.ev_inputs.costs.taker_fee_bps = 4.0;
        candidate.ev_inputs.costs.half_spread_bps = 0.5;
        candidate.ev_inputs.costs.slippage_bps = 0.5;
        candidate.ev_inputs.costs.market_impact_bps = 0.2;
        candidate.ev_inputs.costs.funding_bps_per_hour = 0.5;
        candidate.ev_inputs.costs.expected_holding_hours = expected_holding_hours_;

        return candidate;
    }

    [[nodiscard]] std::string strategy_id() const override { return "sma_cross_demo"; }

private:
    std::string symbol_;
    double target_notional_;
    double expected_holding_hours_;
    SMA<double> fast_{20};
    SMA<double> slow_{50};
    RollingReturnStd vol_{100};
    int last_direction_ = 0;
    int pending_direction_ = 0;
    int confirmation_countdown_ = -1;
};

} // namespace argentum::analysis
