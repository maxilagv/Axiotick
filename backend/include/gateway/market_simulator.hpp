#pragma once

#include "core/types.h"
#include "gateway/smart_order_router.hpp"
#include "gateway/venue.hpp"

#include <cstdint>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

namespace argentum::gateway {

struct LatencyProfile {
    double base_ms = 0.25;
    double jitter_ms = 0.10;
};

struct SimulationVenueState {
    std::string venue_id;
    int64_t displayed_lots = 0;
    int64_t queue_ahead_lots = 0;
    double fill_probability = 1.0;
    double slippage_bps_at_full_take = 5.0;
    LatencyProfile latency{};
};

struct SimulatedFill {
    std::string venue_id;
    int64_t requested_lots = 0;
    int64_t executed_lots = 0;
    int64_t execution_price_ticks = 0;
    int32_t level_index = -1;
    double slippage_bps = 0.0;
    double observed_latency_ms = 0.0;
};

struct SimulationResult {
    int64_t requested_lots = 0;
    int64_t executed_lots = 0;
    int64_t remaining_lots = 0;
    int64_t vwap_price_ticks = 0;
    double avg_slippage_bps = 0.0;
    double p95_latency_ms = 0.0;
    std::vector<SimulatedFill> fills;
};

class MarketExecutionSimulator {
public:
    explicit MarketExecutionSimulator(uint64_t seed = 0xA11CEULL);

    void upsert_venue_state(const SimulationVenueState& state);
    bool get_venue_state(const std::string& venue_id, SimulationVenueState* out) const;

    SimulationResult simulate(
        const Order& order,
        const RoutingDecision& route,
        const std::vector<VenueQuote>& quotes);
    SimulationResult simulate(
        const Order& order,
        const RoutingDecision& route,
        const std::vector<VenueOrderBookSnapshot>& books);
    SimulationResult simulate_with_rerouting(
        const Order& order,
        const SmartOrderRouter& router,
        std::vector<VenueOrderBookSnapshot> books,
        size_t max_passes = 3);

private:
    static int64_t find_quote_price_ticks(const Order& order, const RouteLeg& leg, const std::vector<VenueOrderBookSnapshot>& books);
    static std::vector<VenueOrderBookSnapshot> snapshots_from_quotes(const std::vector<VenueQuote>& quotes);
    static void consume_book_liquidity(
        const Order& order,
        const SimulatedFill& fill,
        std::vector<VenueOrderBookSnapshot>* books);

    mutable std::mt19937_64 rng_;
    std::unordered_map<std::string, SimulationVenueState> venue_states_;
};

} // namespace argentum::gateway
