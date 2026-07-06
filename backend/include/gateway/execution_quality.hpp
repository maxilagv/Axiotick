#pragma once

#include "gateway/market_simulator.hpp"
#include "gateway/smart_order_router.hpp"

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace argentum::gateway {

struct VenueExecutionQuality {
    std::string venue_id;
    int64_t requested_lots = 0;
    int64_t executed_lots = 0;
    double fill_ratio = 0.0;
    double avg_slippage_bps = 0.0;
    double p50_latency_ms = 0.0;
    double p95_latency_ms = 0.0;
};

struct ExecutionQualityReport {
    int64_t total_requested_lots = 0;
    int64_t total_executed_lots = 0;
    double global_fill_ratio = 0.0;
    std::vector<VenueExecutionQuality> venues;
};

class ExecutionQualityTracker {
public:
    void record_route(const RoutingDecision& route);
    void record_fill(const SimulatedFill& fill);
    void record_result(const RoutingDecision& route, const SimulationResult& result);

    ExecutionQualityReport build_report() const;
    void reset();

private:
    struct MutableVenueStats {
        int64_t requested_lots = 0;
        int64_t executed_lots = 0;
        long double slippage_weighted_sum = 0.0;
        std::vector<double> latencies_ms;
    };

    static double percentile(std::vector<double>* values, double quantile);

    mutable std::mutex mutex_;
    std::unordered_map<std::string, MutableVenueStats> by_venue_;
};

std::string execution_quality_report_to_json(const ExecutionQualityReport& report);

} // namespace argentum::gateway
