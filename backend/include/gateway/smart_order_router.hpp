#pragma once

#include "core/types.h"
#include "gateway/venue.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace argentum::gateway {

struct VenueCostProfile {
    std::string venue_id;
    double taker_fee_bps = 0.0;
    double latency_penalty_bps_per_ms = 0.5;
    double expected_latency_ms = 1.0;
};

struct RouteLeg {
    std::string venue_id;
    int64_t requested_lots = 0;
    int64_t quoted_price_ticks = 0;
    int32_t level_index = -1;
    double effective_cost_score = 0.0;
};

struct RoutingDecision {
    bool accepted = false;
    int64_t requested_lots = 0;
    int64_t routed_lots = 0;
    int64_t unrouted_lots = 0;
    std::string reject_reason;
    std::vector<RouteLeg> legs;
};

class SmartOrderRouter {
public:
    SmartOrderRouter() = default;
    explicit SmartOrderRouter(std::vector<VenueCostProfile> profiles);

    void upsert_profile(const VenueCostProfile& profile);
    bool get_profile(const std::string& venue_id, VenueCostProfile* out) const;

    RoutingDecision route(const Order& order, const std::vector<VenueQuote>& quotes) const;
    RoutingDecision route_l2(const Order& order, const std::vector<VenueOrderBookSnapshot>& books) const;
    RoutingDecision reroute_after_partial_fill(
        const Order& original_order,
        int64_t already_executed_lots,
        const std::vector<VenueOrderBookSnapshot>& books,
        const std::vector<std::string>& excluded_venues = {}) const;

private:
    static bool level_is_eligible(const Order& order, int64_t price_ticks, int64_t size_lots);
    static double effective_cost_score(const Order& order, int64_t price_ticks, const VenueCostProfile& profile);

    std::unordered_map<std::string, VenueCostProfile> profiles_;
};

} // namespace argentum::gateway
