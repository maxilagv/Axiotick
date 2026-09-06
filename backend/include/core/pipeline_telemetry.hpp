#pragma once

// Aggregates TraceSpans into per-stage latency histograms and the
// wire-to-decision report (ADR 0014).
//
// absorb() runs on the driver thread after each tick's decision completes —
// off the per-stage hot path, but still allocation-free and lock-free.
// Reporting (print/JSON) is strictly end-of-run.

#include "core/latency_histogram.hpp"
#include "core/trace_context.hpp"

#include <cstdio>
#include <string>

namespace argentum::core {

struct PipelineLatencyReport {
    uint64_t ticks_absorbed = 0;
    /// Delta from the previous stamped stage to this one, per stage.
    std::array<LatencyReport, kTraceStageCount> stage{};
    /// TickIngress -> GateVerdict (every candidate that reached the gate).
    LatencyReport wire_to_gate{};
    /// TickIngress -> OmsAccept when an order was accepted, otherwise
    /// TickIngress -> GateVerdict: "how long until the decision was final".
    LatencyReport wire_to_decision{};
};

class PipelineTelemetry {
public:
    /// Records stage-to-stage deltas for every stamped stage of one tick.
    void absorb(const TraceSpans& spans);

    [[nodiscard]] PipelineLatencyReport report() const;

    /// Human-readable table (demo/operator output).
    void print(std::FILE* out) const;

    /// Writes the report as JSON next to the run's other artifacts.
    /// Returns false on I/O failure. Never called on a hot path.
    bool write_json(const std::string& path, const std::string& run_label) const;

    void reset();

private:
    uint64_t ticks_absorbed_ = 0;
    std::array<LatencyHistogram, kTraceStageCount> stage_{};
    LatencyHistogram wire_to_gate_{};
    LatencyHistogram wire_to_decision_{};
};

} // namespace argentum::core
