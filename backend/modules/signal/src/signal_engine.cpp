#include "signal/signal_engine.hpp"

#include "audit/logger.hpp"
#include "core/fixed_point.hpp"
#include "core/time_utils.hpp"
#include "trading/order_manager.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace argentum::signal {

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

std::string format_trace_spans(const core::TraceSpans& trace) {
    // Compact "stage=mono_ns;..." payload: only stamped stages, canonical order.
    std::string out;
    out.reserve(160);
    for (size_t i = 0; i < core::kTraceStageCount; ++i) {
        const uint64_t at = trace.mono_ns[i];
        if (at == 0) continue;
        if (!out.empty()) out.push_back(';');
        out += core::to_string(static_cast<core::TraceStage>(i));
        out.push_back('=');
        out += format_u64(at);
    }
    return out;
}

} // namespace

SignalEngine::SignalEngine(Config config, std::shared_ptr<trading::OrderManager> oms)
    : config_(config),
      gate_(config.gate),
      oms_(std::move(oms)),
      registry_(config.registry),
      next_signal_id_(config.first_signal_id),
      next_order_id_(config.first_order_id) {}

Signal SignalEngine::process(const SignalCandidate& candidate, core::TraceSpans* trace) {
    if (trace) {
        trace->stamp(core::TraceStage::SignalEvalStart);
    }

    Signal signal{};
    signal.candidate = candidate;
    signal.signal_id = next_signal_id_.fetch_add(1, std::memory_order_relaxed);
    if (signal.candidate.timestamp_ns == 0) {
        signal.candidate.timestamp_ns = core::wall_now_ns();
    }
    if (signal.candidate.origin_tick_id == 0 && trace) {
        signal.candidate.origin_tick_id = trace->tick_id;
    }

    signal.lifecycle_state = strategy_state(signal.candidate.strategy_id);

    const uint64_t gate_start_ns = core::mono_now_ns();
    signal.ev_result = gate_.evaluate(
        signal.candidate.ev_inputs,
        signal.candidate.regime,
        signal.lifecycle_state);
    record_gate_latency(core::mono_now_ns() - gate_start_ns);
    if (trace) {
        trace->stamp(core::TraceStage::GateVerdict);
    }

    if (signal.ev_result.accepted) {
        signal.submitted_quantity = signal.candidate.quantity * signal.ev_result.size_multiplier;

        if (oms_) {
            Order order{};
            order.order_id = next_order_id_.fetch_add(1, std::memory_order_relaxed);
            order.timestamp_ns = signal.candidate.timestamp_ns;
            order.price = signal.candidate.price;
            order.quantity = signal.submitted_quantity;
            std::strncpy(order.symbol, signal.candidate.symbol.c_str(), sizeof(order.symbol) - 1);
            order.side = signal.candidate.side;
            order.type = signal.candidate.order_type;
            order.tif = signal.candidate.tif;

            signal.submitted_order_id = order.order_id;
            const trading::OrderSubmissionResult submission =
                oms_->submit_order(order, signal.signal_id, trace);
            signal.order_accepted = submission.accepted;

            // Register every immediate fill for horizon evaluation: the
            // signal promised ev_net_bps over expected_holding_hours; the
            // registry scores that promise when the horizon elapses.
            if (submission.accepted) {
                for (const Trade& trade : submission.trades) {
                    const double fill_price = core::from_price_ticks(trade.price_ticks);
                    const double fill_qty = core::from_quantity_lots(trade.quantity_lots);
                    registry_.record_signal_open(
                        signal.signal_id,
                        signal.candidate.strategy_id,
                        signal.candidate.symbol,
                        signal.candidate.side,
                        fill_price,
                        fill_price * fill_qty,
                        signal.ev_result.ev_net_bps,
                        signal.candidate.ev_inputs.costs.expected_holding_hours,
                        signal.candidate.regime,
                        signal.candidate.timestamp_ns);
                }
            }
        }
    }

    signal.decision_reason = build_decision_reason(signal);
    audit_decision(signal, trace);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++stats_.candidates;
        if (signal.ev_result.accepted) {
            ++stats_.gate_accepted;
            if (signal.submitted_order_id != 0) {
                ++stats_.orders_submitted;
                if (signal.order_accepted) {
                    ++stats_.orders_accepted;
                }
            }
        } else {
            const size_t reason_index = static_cast<size_t>(signal.ev_result.reject_reason);
            if (reason_index < stats_.rejects_by_reason.size()) {
                ++stats_.rejects_by_reason[reason_index];
            }
        }
    }

    return signal;
}

void SignalEngine::on_market_tick(const std::string& symbol, double price, uint64_t timestamp_ns) {
    registry_.on_market_tick(symbol, price, timestamp_ns);
}

void SignalEngine::set_strategy_state(const std::string& strategy_id, ev::StrategyState state) {
    registry_.force_state(strategy_id, state, "signal_engine_override");
}

ev::StrategyState SignalEngine::strategy_state(const std::string& strategy_id) const {
    return registry_.state(strategy_id);
}

SignalEngineStats SignalEngine::stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
}

GateLatencySnapshot SignalEngine::gate_latency_snapshot() const {
    core::LatencyReport report{};
    {
        std::lock_guard<std::mutex> lock(mutex_);
        report = gate_latency_hist_.report();
    }

    GateLatencySnapshot snapshot{};
    snapshot.samples = report.samples;
    snapshot.p50_ns = report.p50_ns;
    snapshot.p95_ns = report.p95_ns;
    snapshot.p99_ns = report.p99_ns;
    snapshot.p999_ns = report.p999_ns;
    snapshot.max_ns = report.max_ns;
    return snapshot;
}

void SignalEngine::record_gate_latency(uint64_t latency_ns) {
    std::lock_guard<std::mutex> lock(mutex_);
    gate_latency_hist_.record(latency_ns);
}

void SignalEngine::audit_decision(const Signal& signal, const core::TraceSpans* trace) const {
    const SignalCandidate& c = signal.candidate;
    const ev::EVInputs& in = c.ev_inputs;
    const ev::EVResult& r = signal.ev_result;

    // Per-stage spans are audited for every reject and for 1-in-N accepted
    // decisions: tails stay observable without inflating the audit log.
    std::string trace_spans;
    if (trace) {
        const uint32_t every = (config_.trace_audit_sample_every == 0)
            ? 1
            : config_.trace_audit_sample_every;
        if (!r.accepted || (signal.signal_id % every) == 0) {
            trace_spans = format_trace_spans(*trace);
        }
    }

    audit::Logger::instance().structured_log(
        audit::LogLevel::AUDIT,
        "signal_decision",
        {{"signal_id", format_u64(signal.signal_id)},
         {"origin_tick_id", format_u64(c.origin_tick_id)},
         {"trace_spans_mono_ns", trace_spans},
         {"signal_timestamp_ns", format_u64(c.timestamp_ns)},
         {"strategy_id", c.strategy_id},
         {"model_version", c.model_version},
         {"feature_snapshot_id", c.feature_snapshot_id},
         {"symbol", c.symbol},
         {"side", (c.side == SIDE_BUY) ? "buy" : "sell"},
         {"price", format_double(c.price)},
         {"candidate_quantity", format_double(c.quantity)},
         {"submitted_quantity", format_double(signal.submitted_quantity)},
         {"notional", format_double(in.notional)},
         {"regime", regime::to_string(c.regime)},
         {"regime_confidence", format_double(c.regime_confidence)},
         {"lifecycle_state", ev::to_string(signal.lifecycle_state)},
         {"p_win", format_double(in.p_win)},
         {"avg_win_bps", format_double(in.avg_win_bps)},
         {"avg_loss_bps", format_double(in.avg_loss_bps)},
         {"confidence", format_double(in.confidence)},
         {"taker_fee_bps", format_double(in.costs.taker_fee_bps)},
         {"half_spread_bps", format_double(in.costs.half_spread_bps)},
         {"slippage_bps", format_double(in.costs.slippage_bps)},
         {"market_impact_bps", format_double(in.costs.market_impact_bps)},
         {"funding_bps_per_hour", format_double(in.costs.funding_bps_per_hour)},
         {"expected_holding_hours", format_double(in.costs.expected_holding_hours)},
         {"ev_gross_bps", format_double(r.ev_gross_bps)},
         {"cost_bps", format_double(r.cost_bps)},
         {"ev_net_bps", format_double(r.ev_net_bps)},
         {"ev_net_notional", format_double(r.ev_net_notional)},
         {"effective_threshold_bps", format_double(r.effective_threshold_bps)},
         {"size_multiplier", format_double(r.size_multiplier)},
         {"accepted", r.accepted ? "true" : "false"},
         {"reject_reason", ev::to_string(r.reject_reason)},
         {"order_id", format_u64(signal.submitted_order_id)},
         {"order_accepted", signal.order_accepted ? "true" : "false"},
         {"decision_reason", signal.decision_reason}});
}

std::string SignalEngine::build_decision_reason(const Signal& signal) {
    const ev::EVResult& r = signal.ev_result;
    char buffer[256];

    if (r.accepted) {
        std::snprintf(
            buffer,
            sizeof(buffer),
            "accepted: ev_net=%.2fbps >= threshold=%.2fbps (regime=%s, state=%s, size_mult=%.2f)",
            r.ev_net_bps,
            r.effective_threshold_bps,
            regime::to_string(signal.candidate.regime),
            ev::to_string(signal.lifecycle_state),
            r.size_multiplier);
    } else {
        std::snprintf(
            buffer,
            sizeof(buffer),
            "rejected: %s (ev_net=%.2fbps, threshold=%.2fbps, confidence=%.2f, regime=%s, state=%s)",
            ev::to_string(r.reject_reason),
            r.ev_net_bps,
            r.effective_threshold_bps,
            signal.candidate.ev_inputs.confidence,
            regime::to_string(signal.candidate.regime),
            ev::to_string(signal.lifecycle_state));
    }

    return std::string(buffer);
}

} // namespace argentum::signal
