#include "gateway/market_simulator.hpp"

#include "core/fixed_point.hpp"

#include <algorithm>
#include <cmath>

namespace argentum::gateway {

namespace {
SimulationVenueState default_state_for_leg(const RouteLeg& leg) {
    SimulationVenueState state{};
    state.venue_id = leg.venue_id;
    state.displayed_lots = leg.requested_lots;
    state.queue_ahead_lots = 0;
    state.fill_probability = 1.0;
    state.slippage_bps_at_full_take = 1.0;
    state.latency = LatencyProfile{};
    return state;
}

void finalize_result_metrics(SimulationResult* result) {
    if (!result) return;

    long double weighted_price = 0.0;
    long double weighted_slippage = 0.0;
    std::vector<double> latencies;
    latencies.reserve(result->fills.size());

    for (const auto& fill : result->fills) {
        latencies.push_back(fill.observed_latency_ms);
        if (fill.executed_lots <= 0) continue;
        weighted_price += static_cast<long double>(fill.execution_price_ticks) *
            static_cast<long double>(fill.executed_lots);
        weighted_slippage += static_cast<long double>(fill.slippage_bps) *
            static_cast<long double>(fill.executed_lots);
    }

    if (result->executed_lots > 0) {
        result->vwap_price_ticks = static_cast<int64_t>(
            std::llround(weighted_price / static_cast<long double>(result->executed_lots)));
        result->avg_slippage_bps = static_cast<double>(
            weighted_slippage / static_cast<long double>(result->executed_lots));
    }

    if (!latencies.empty()) {
        std::sort(latencies.begin(), latencies.end());
        const size_t idx = static_cast<size_t>(
            std::floor(0.95 * static_cast<double>(latencies.size() - 1)));
        result->p95_latency_ms = latencies[idx];
    }
}
} // namespace

MarketExecutionSimulator::MarketExecutionSimulator(uint64_t seed) : rng_(seed) {}

void MarketExecutionSimulator::upsert_venue_state(const SimulationVenueState& state) {
    if (state.venue_id.empty()) return;
    venue_states_[state.venue_id] = state;
}

bool MarketExecutionSimulator::get_venue_state(const std::string& venue_id, SimulationVenueState* out) const {
    if (!out) return false;
    const auto it = venue_states_.find(venue_id);
    if (it == venue_states_.end()) return false;
    *out = it->second;
    return true;
}

SimulationResult MarketExecutionSimulator::simulate(
    const Order& order,
    const RoutingDecision& route,
    const std::vector<VenueQuote>& quotes) {
    return simulate(order, route, snapshots_from_quotes(quotes));
}

SimulationResult MarketExecutionSimulator::simulate(
    const Order& order,
    const RoutingDecision& route,
    const std::vector<VenueOrderBookSnapshot>& books) {
    SimulationResult result{};
    result.requested_lots = route.requested_lots;
    result.remaining_lots = route.requested_lots;

    if (!route.accepted) {
        return result;
    }

    std::uniform_real_distribution<double> unit(0.0, 1.0);

    for (const auto& leg : route.legs) {
        if (leg.requested_lots <= 0) continue;

        SimulationVenueState state{};
        auto it = venue_states_.find(leg.venue_id);
        if (it != venue_states_.end()) {
            state = it->second;
        } else {
            state = default_state_for_leg(leg);
        }

        const int64_t book_price_ticks = find_quote_price_ticks(order, leg, books);
        const int64_t effective_price_ticks = (book_price_ticks > 0) ? book_price_ticks : leg.quoted_price_ticks;
        if (effective_price_ticks <= 0) continue;

        const int64_t available_after_queue = std::max<int64_t>(0, state.displayed_lots - state.queue_ahead_lots);
        const int64_t capped_request = std::min<int64_t>(leg.requested_lots, available_after_queue);
        const double fill_prob = std::clamp(state.fill_probability, 0.0, 1.0);

        int64_t executed_lots = 0;
        if (capped_request > 0 && unit(rng_) <= fill_prob) {
            const double microfill = 0.90 + (0.10 * unit(rng_));
            executed_lots = static_cast<int64_t>(
                std::llround(static_cast<double>(capped_request) * microfill));
            executed_lots = std::clamp<int64_t>(executed_lots, 0, capped_request);
        }

        const double latency_ms = state.latency.base_ms + (unit(rng_) * state.latency.jitter_ms);
        if (executed_lots <= 0) {
            result.fills.push_back(SimulatedFill{
                leg.venue_id,
                leg.requested_lots,
                0,
                effective_price_ticks,
                leg.level_index,
                0.0,
                latency_ms
            });
            continue;
        }

        const double participation = state.displayed_lots > 0
            ? static_cast<double>(executed_lots) / static_cast<double>(state.displayed_lots)
            : 1.0;
        const double slippage_bps = std::max(0.0, state.slippage_bps_at_full_take * participation);

        int64_t exec_price_ticks = effective_price_ticks;
        const double multiplier = slippage_bps / 10'000.0;
        if (order.side == SIDE_BUY) {
            exec_price_ticks = static_cast<int64_t>(
                std::llround(static_cast<double>(effective_price_ticks) * (1.0 + multiplier)));
        } else if (order.side == SIDE_SELL) {
            exec_price_ticks = static_cast<int64_t>(
                std::llround(static_cast<double>(effective_price_ticks) * (1.0 - multiplier)));
        }

        result.fills.push_back(SimulatedFill{
            leg.venue_id,
            leg.requested_lots,
            executed_lots,
            exec_price_ticks,
            leg.level_index,
            slippage_bps,
            latency_ms
        });

        result.executed_lots += executed_lots;
        result.remaining_lots = std::max<int64_t>(0, result.requested_lots - result.executed_lots);
    }

    finalize_result_metrics(&result);
    return result;
}

SimulationResult MarketExecutionSimulator::simulate_with_rerouting(
    const Order& order,
    const SmartOrderRouter& router,
    std::vector<VenueOrderBookSnapshot> books,
    size_t max_passes) {
    Order normalized = order;
    core::normalize_order_scalars(&normalized);

    SimulationResult aggregate{};
    aggregate.requested_lots = normalized.quantity_lots;
    aggregate.remaining_lots = normalized.quantity_lots;

    if (normalized.order_id == 0 || normalized.quantity_lots <= 0 || max_passes == 0) {
        return aggregate;
    }

    int64_t executed_so_far = 0;
    for (size_t pass = 0; pass < max_passes; ++pass) {
        const int64_t remaining_lots = std::max<int64_t>(0, normalized.quantity_lots - executed_so_far);
        if (remaining_lots <= 0) break;

        Order residual = normalized;
        residual.quantity_lots = remaining_lots;
        residual.quantity = core::from_quantity_lots(remaining_lots);

        RoutingDecision route = (pass == 0)
            ? router.route_l2(residual, books)
            : router.reroute_after_partial_fill(normalized, executed_so_far, books);

        if (!route.accepted || route.routed_lots <= 0) {
            break;
        }

        SimulationResult pass_result = simulate(residual, route, books);
        aggregate.fills.insert(
            aggregate.fills.end(),
            pass_result.fills.begin(),
            pass_result.fills.end());

        for (const auto& fill : pass_result.fills) {
            consume_book_liquidity(residual, fill, &books);
        }

        aggregate.executed_lots += pass_result.executed_lots;
        executed_so_far = aggregate.executed_lots;
        aggregate.remaining_lots = std::max<int64_t>(0, normalized.quantity_lots - executed_so_far);

        if (pass_result.executed_lots <= 0) {
            break;
        }
        if (normalized.tif == TIF_IOC || normalized.tif == TIF_FOK) {
            break;
        }
    }

    finalize_result_metrics(&aggregate);
    return aggregate;
}

int64_t MarketExecutionSimulator::find_quote_price_ticks(
    const Order& order,
    const RouteLeg& leg,
    const std::vector<VenueOrderBookSnapshot>& books) {
    for (const auto& book : books) {
        if (book.venue_id != leg.venue_id) continue;

        const auto& levels = (order.side == SIDE_BUY) ? book.ask_levels : book.bid_levels;
        if (leg.level_index >= 0 && static_cast<size_t>(leg.level_index) < levels.size()) {
            return levels[static_cast<size_t>(leg.level_index)].price_ticks;
        }
        if (!levels.empty()) {
            return levels.front().price_ticks;
        }
    }
    return 0;
}

std::vector<VenueOrderBookSnapshot> MarketExecutionSimulator::snapshots_from_quotes(
    const std::vector<VenueQuote>& quotes) {
    std::vector<VenueOrderBookSnapshot> books;
    books.reserve(quotes.size());
    for (const auto& quote : quotes) {
        books.push_back(snapshot_from_top_of_book(quote));
    }
    return books;
}

void MarketExecutionSimulator::consume_book_liquidity(
    const Order& order,
    const SimulatedFill& fill,
    std::vector<VenueOrderBookSnapshot>* books) {
    if (!books || fill.executed_lots <= 0) return;

    for (auto& book : *books) {
        if (book.venue_id != fill.venue_id) continue;

        auto& levels = (order.side == SIDE_BUY) ? book.ask_levels : book.bid_levels;
        if (levels.empty()) return;

        size_t idx = 0;
        if (fill.level_index >= 0 && static_cast<size_t>(fill.level_index) < levels.size()) {
            idx = static_cast<size_t>(fill.level_index);
        }

        int64_t remaining = fill.executed_lots;
        while (remaining > 0 && idx < levels.size()) {
            QuoteLevel& level = levels[idx];
            if (level.size_lots <= 0) {
                ++idx;
                continue;
            }

            const int64_t take = std::min<int64_t>(remaining, level.size_lots);
            level.size_lots -= take;
            remaining -= take;

            if (level.size_lots <= 0) {
                ++idx;
            }
        }

        levels.erase(
            std::remove_if(levels.begin(), levels.end(), [](const QuoteLevel& lvl) {
                return lvl.size_lots <= 0;
            }),
            levels.end());
        return;
    }
}

} // namespace argentum::gateway
