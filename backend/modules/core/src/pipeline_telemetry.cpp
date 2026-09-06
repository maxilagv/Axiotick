#include "core/pipeline_telemetry.hpp"

#include <cinttypes>
#include <filesystem>
#include <fstream>

namespace argentum::core {

void PipelineTelemetry::absorb(const TraceSpans& spans) {
    const uint64_t ingress = spans.at(TraceStage::TickIngress);
    if (ingress == 0) {
        return;  // nothing to attribute without an ingress anchor
    }

    ++ticks_absorbed_;

    uint64_t previous = ingress;
    for (size_t i = 1; i < kTraceStageCount; ++i) {
        const uint64_t at = spans.mono_ns[i];
        if (at == 0) continue;
        if (at >= previous) {
            stage_[i].record(at - previous);
        }
        previous = at;
    }

    const uint64_t gate = spans.at(TraceStage::GateVerdict);
    if (gate >= ingress && gate != 0) {
        wire_to_gate_.record(gate - ingress);
    }

    const uint64_t oms = spans.at(TraceStage::OmsAccept);
    const uint64_t decision_end = (oms != 0) ? oms : gate;
    if (decision_end >= ingress && decision_end != 0) {
        wire_to_decision_.record(decision_end - ingress);
    }
}

PipelineLatencyReport PipelineTelemetry::report() const {
    PipelineLatencyReport out{};
    out.ticks_absorbed = ticks_absorbed_;
    for (size_t i = 0; i < kTraceStageCount; ++i) {
        out.stage[i] = stage_[i].report();
    }
    out.wire_to_gate = wire_to_gate_.report();
    out.wire_to_decision = wire_to_decision_.report();
    return out;
}

namespace {

void print_row(std::FILE* out, const char* label, const LatencyReport& r) {
    if (r.samples == 0) return;
    std::fprintf(out,
                 "%-18s samples=%-8" PRIu64 " p50=%-8" PRIu64 " p95=%-8" PRIu64 " p99=%-8" PRIu64
                 " p99.9=%-8" PRIu64 " max=%-10" PRIu64 " (ns)\n",
                 label, r.samples, r.p50_ns, r.p95_ns, r.p99_ns, r.p999_ns, r.max_ns);
}

void json_row(std::ofstream& os, const char* label, const LatencyReport& r, bool* first) {
    if (!*first) {
        os << ",";
    }
    *first = false;
    os << "\n    {\"stage\":\"" << label << "\",\"samples\":" << r.samples
       << ",\"min_ns\":" << r.min_ns << ",\"p50_ns\":" << r.p50_ns << ",\"p95_ns\":" << r.p95_ns
       << ",\"p99_ns\":" << r.p99_ns << ",\"p999_ns\":" << r.p999_ns << ",\"max_ns\":" << r.max_ns
       << ",\"mean_ns\":" << r.mean_ns << "}";
}

} // namespace

void PipelineTelemetry::print(std::FILE* out) const {
    const PipelineLatencyReport r = report();
    std::fprintf(out, "--- wire-to-decision latency (ticks=%" PRIu64 ") ---\n", r.ticks_absorbed);
    if (r.ticks_absorbed == 0) {
        std::fprintf(out, "(no ticks carried a trace context)\n");
        return;
    }
    for (size_t i = 1; i < kTraceStageCount; ++i) {
        print_row(out, to_string(static_cast<TraceStage>(i)), r.stage[i]);
    }
    print_row(out, "wire->gate", r.wire_to_gate);
    print_row(out, "wire->decision", r.wire_to_decision);
}

bool PipelineTelemetry::write_json(const std::string& path, const std::string& run_label) const {
    std::error_code ec;
    const std::filesystem::path fs_path(path);
    if (!fs_path.parent_path().empty()) {
        std::filesystem::create_directories(fs_path.parent_path(), ec);
    }

    std::ofstream os(path, std::ios::out | std::ios::trunc);
    if (!os.is_open()) {
        return false;
    }

    const PipelineLatencyReport r = report();
    const ClockCalibration calibration = ClockCalibration::measure();

    os << "{\n  \"run_label\":\"" << run_label << "\",";
    os << "\n  \"generated_utc\":\"" << to_utc(wall_now_ns()) << "\",";
    os << "\n  \"clock_uncertainty_ns\":" << calibration.uncertainty_ns << ",";
    os << "\n  \"ticks_absorbed\":" << r.ticks_absorbed << ",";
    os << "\n  \"stages\":[";
    bool first = true;
    for (size_t i = 1; i < kTraceStageCount; ++i) {
        if (r.stage[i].samples == 0) continue;
        json_row(os, to_string(static_cast<TraceStage>(i)), r.stage[i], &first);
    }
    json_row(os, "wire_to_gate", r.wire_to_gate, &first);
    json_row(os, "wire_to_decision", r.wire_to_decision, &first);
    os << "\n  ]\n}\n";
    return os.good();
}

void PipelineTelemetry::reset() {
    ticks_absorbed_ = 0;
    for (auto& hist : stage_) {
        hist.reset();
    }
    wire_to_gate_.reset();
    wire_to_decision_.reset();
}

} // namespace argentum::core
