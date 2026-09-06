#include "core/trace_context.hpp"

#include <atomic>

namespace argentum::core {

namespace {
std::atomic<uint64_t> g_next_tick_id{1};
} // namespace

const char* to_string(TraceStage stage) {
    switch (stage) {
        case TraceStage::TickIngress: return "tick_ingress";
        case TraceStage::Decode: return "decode";
        case TraceStage::BusPublish: return "bus_publish";
        case TraceStage::BusConsume: return "bus_consume";
        case TraceStage::BarClose: return "bar_close";
        case TraceStage::SignalEvalStart: return "signal_eval_start";
        case TraceStage::GateVerdict: return "gate_verdict";
        case TraceStage::RiskVerdict: return "risk_verdict";
        case TraceStage::JournalEnqueue: return "journal_enqueue";
        case TraceStage::OmsAccept: return "oms_accept";
        default: return "unknown";
    }
}

uint64_t next_tick_id() {
    return g_next_tick_id.fetch_add(1, std::memory_order_relaxed);
}

} // namespace argentum::core
