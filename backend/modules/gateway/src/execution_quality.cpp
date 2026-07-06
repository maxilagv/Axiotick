#include "gateway/execution_quality.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace argentum::gateway {

void ExecutionQualityTracker::record_route(const RoutingDecision& route) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& leg : route.legs) {
        if (leg.venue_id.empty() || leg.requested_lots <= 0) continue;
        by_venue_[leg.venue_id].requested_lots += leg.requested_lots;
    }
}

void ExecutionQualityTracker::record_fill(const SimulatedFill& fill) {
    if (fill.venue_id.empty() || fill.requested_lots < 0 || fill.executed_lots < 0) return;

    std::lock_guard<std::mutex> lock(mutex_);
    MutableVenueStats& stats = by_venue_[fill.venue_id];
    stats.executed_lots += fill.executed_lots;
    if (fill.executed_lots > 0) {
        stats.slippage_weighted_sum +=
            static_cast<long double>(fill.slippage_bps) * static_cast<long double>(fill.executed_lots);
    }
    stats.latencies_ms.push_back(fill.observed_latency_ms);
}

void ExecutionQualityTracker::record_result(const RoutingDecision& route, const SimulationResult& result) {
    record_route(route);
    for (const auto& fill : result.fills) {
        record_fill(fill);
    }
}

ExecutionQualityReport ExecutionQualityTracker::build_report() const {
    std::lock_guard<std::mutex> lock(mutex_);

    ExecutionQualityReport report{};
    report.venues.reserve(by_venue_.size());

    for (const auto& [venue_id, stats] : by_venue_) {
        VenueExecutionQuality out{};
        out.venue_id = venue_id;
        out.requested_lots = stats.requested_lots;
        out.executed_lots = stats.executed_lots;
        out.fill_ratio = (stats.requested_lots > 0)
            ? static_cast<double>(stats.executed_lots) / static_cast<double>(stats.requested_lots)
            : 0.0;
        out.avg_slippage_bps = (stats.executed_lots > 0)
            ? static_cast<double>(stats.slippage_weighted_sum / static_cast<long double>(stats.executed_lots))
            : 0.0;

        std::vector<double> latencies = stats.latencies_ms;
        out.p50_latency_ms = percentile(&latencies, 0.50);
        out.p95_latency_ms = percentile(&latencies, 0.95);

        report.total_requested_lots += out.requested_lots;
        report.total_executed_lots += out.executed_lots;
        report.venues.push_back(out);
    }

    std::sort(report.venues.begin(), report.venues.end(), [](const auto& a, const auto& b) {
        return a.venue_id < b.venue_id;
    });

    report.global_fill_ratio = (report.total_requested_lots > 0)
        ? static_cast<double>(report.total_executed_lots) / static_cast<double>(report.total_requested_lots)
        : 0.0;

    return report;
}

void ExecutionQualityTracker::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    by_venue_.clear();
}

double ExecutionQualityTracker::percentile(std::vector<double>* values, double quantile) {
    if (!values || values->empty()) return 0.0;

    quantile = std::clamp(quantile, 0.0, 1.0);
    std::sort(values->begin(), values->end());
    const size_t idx = static_cast<size_t>(
        std::llround(quantile * static_cast<double>(values->size() - 1)));
    return (*values)[idx];
}

std::string execution_quality_report_to_json(const ExecutionQualityReport& report) {
    std::ostringstream os;
    os.setf(std::ios::fixed);
    os.precision(6);

    os << "{\"total_requested_lots\":" << report.total_requested_lots
       << ",\"total_executed_lots\":" << report.total_executed_lots
       << ",\"global_fill_ratio\":" << report.global_fill_ratio
       << ",\"venues\":[";

    for (size_t i = 0; i < report.venues.size(); ++i) {
        const auto& v = report.venues[i];
        if (i != 0) os << ",";
        os << "{\"venue_id\":\"" << v.venue_id
           << "\",\"requested_lots\":" << v.requested_lots
           << ",\"executed_lots\":" << v.executed_lots
           << ",\"fill_ratio\":" << v.fill_ratio
           << ",\"avg_slippage_bps\":" << v.avg_slippage_bps
           << ",\"p50_latency_ms\":" << v.p50_latency_ms
           << ",\"p95_latency_ms\":" << v.p95_latency_ms
           << "}";
    }

    os << "]}";
    return os.str();
}

} // namespace argentum::gateway
