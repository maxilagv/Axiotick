#pragma once

#include "ev/ev_gate.hpp"
#include "signal/signal_types.hpp"
#include "strategy/strategy_registry.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace argentum::trading {
class OrderManager;
}

namespace argentum::signal {

struct SignalEngineStats {
    uint64_t candidates = 0;
    uint64_t gate_accepted = 0;
    uint64_t orders_submitted = 0;
    uint64_t orders_accepted = 0;
    std::array<uint64_t, ev::kRejectReasonCount> rejects_by_reason{};
};

struct GateLatencySnapshot {
    uint64_t samples = 0;
    uint64_t p50_ns = 0;
    uint64_t p95_ns = 0;
    uint64_t p99_ns = 0;
    uint64_t max_ns = 0;
};

/**
 * @brief Orchestrates candidate -> lifecycle -> EV gate -> OMS.
 *
 * Runs upstream of OrderManager::submit_order(); RiskManager keeps enforcing
 * every capital check unchanged downstream. Every decision (accepted or not)
 * is written to the structured audit log as a "signal_decision" event carrying
 * the full explainability payload, and accepted orders are journaled with
 * related_signal_id for replay correlation.
 *
 * Strategy lifecycle is governed by the embedded StrategyRegistry (Block 3):
 * every accepted fill is recorded for horizon evaluation, market ticks fed
 * via on_market_tick() resolve those evaluations, and the registry's
 * statistical tests (Page-Hinkley, rolling-Sharpe floor, EV decay, strategy
 * drawdown) drive automatic transitions that the EV gate enforces on the
 * very next candidate. set_strategy_state() remains as the manual override.
 */
class SignalEngine {
public:
    struct Config {
        ev::EVGateConfig gate{};
        strategy::StrategyRegistryConfig registry{};
        uint64_t first_signal_id = 1;
        uint64_t first_order_id = 1'000'000;  // demo/local default; compose with the API id generator in service builds
    };

    SignalEngine(Config config, std::shared_ptr<trading::OrderManager> oms);

    /// Evaluates one candidate end-to-end. Thread-safe.
    Signal process(const SignalCandidate& candidate);

    /// Feeds a market tick to the lifecycle registry so pending horizon
    /// evaluations of `symbol` can resolve. Call alongside mark_to_market.
    void on_market_tick(const std::string& symbol, double price, uint64_t timestamp_ns);

    /// Manual lifecycle override (audited by the registry).
    void set_strategy_state(const std::string& strategy_id, ev::StrategyState state);
    [[nodiscard]] ev::StrategyState strategy_state(const std::string& strategy_id) const;

    [[nodiscard]] strategy::StrategyRegistry& registry() { return registry_; }
    [[nodiscard]] const strategy::StrategyRegistry& registry() const { return registry_; }

    [[nodiscard]] SignalEngineStats stats() const;
    [[nodiscard]] GateLatencySnapshot gate_latency_snapshot() const;
    [[nodiscard]] const ev::EVGate& gate() const { return gate_; }

private:
    void record_gate_latency(uint64_t latency_ns);
    void audit_decision(const Signal& signal) const;
    static std::string build_decision_reason(const Signal& signal);

    Config config_;
    ev::EVGate gate_;
    std::shared_ptr<trading::OrderManager> oms_;
    strategy::StrategyRegistry registry_;
    std::atomic<uint64_t> next_signal_id_;
    std::atomic<uint64_t> next_order_id_;

    mutable std::mutex mutex_;
    SignalEngineStats stats_{};
    std::vector<uint64_t> gate_latency_samples_;
};

} // namespace argentum::signal
