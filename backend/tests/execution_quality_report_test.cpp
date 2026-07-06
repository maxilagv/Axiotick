#include "gateway/execution_quality.hpp"

#include <string>

#define CHECK(cond) do { if (!(cond)) return 1; } while (0)

namespace {
const argentum::gateway::VenueExecutionQuality* find_venue(
    const argentum::gateway::ExecutionQualityReport& report,
    const std::string& venue_id) {
    for (const auto& v : report.venues) {
        if (v.venue_id == venue_id) return &v;
    }
    return nullptr;
}
}

int main() {
    argentum::gateway::ExecutionQualityTracker tracker;

    argentum::gateway::RoutingDecision route{};
    route.accepted = true;
    route.requested_lots = 5;
    route.routed_lots = 5;
    route.legs.push_back(argentum::gateway::RouteLeg{"V1", 2, 10000, 0, 10001.0});
    route.legs.push_back(argentum::gateway::RouteLeg{"V2", 3, 10002, 0, 10003.0});

    argentum::gateway::SimulationResult sim{};
    sim.fills.push_back(argentum::gateway::SimulatedFill{"V1", 1, 1, 10001, 0, 2.0, 1.0});
    sim.fills.push_back(argentum::gateway::SimulatedFill{"V1", 1, 1, 10002, 1, 4.0, 2.0});
    sim.fills.push_back(argentum::gateway::SimulatedFill{"V2", 2, 2, 10003, 0, 1.0, 3.0});
    sim.fills.push_back(argentum::gateway::SimulatedFill{"V2", 1, 0, 10004, 1, 0.0, 4.0});

    tracker.record_result(route, sim);
    auto report = tracker.build_report();

    CHECK(report.total_requested_lots == 5);
    CHECK(report.total_executed_lots == 4);
    CHECK(report.global_fill_ratio > 0.79 && report.global_fill_ratio < 0.81);

    const auto* v1 = find_venue(report, "V1");
    const auto* v2 = find_venue(report, "V2");
    CHECK(v1 != nullptr);
    CHECK(v2 != nullptr);

    CHECK(v1->requested_lots == 2);
    CHECK(v1->executed_lots == 2);
    CHECK(v1->fill_ratio > 0.99);
    CHECK(v1->avg_slippage_bps > 2.9 && v1->avg_slippage_bps < 3.1);
    CHECK(v1->p50_latency_ms == 2.0);
    CHECK(v1->p95_latency_ms == 2.0);

    CHECK(v2->requested_lots == 3);
    CHECK(v2->executed_lots == 2);
    CHECK(v2->fill_ratio > 0.66 && v2->fill_ratio < 0.67);
    CHECK(v2->avg_slippage_bps > 0.99 && v2->avg_slippage_bps < 1.01);
    CHECK(v2->p95_latency_ms == 4.0);

    const std::string json = argentum::gateway::execution_quality_report_to_json(report);
    CHECK(json.find("\"venue_id\":\"V1\"") != std::string::npos);
    CHECK(json.find("\"global_fill_ratio\":") != std::string::npos);

    return 0;
}
