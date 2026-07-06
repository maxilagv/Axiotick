#include "gateway/smart_order_router.hpp"

#include "core/fixed_point.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_set>

namespace argentum::gateway {

namespace {
struct RankedLevel {
    const VenueOrderBookSnapshot* book = nullptr;
    VenueCostProfile profile{};
    int32_t level_index = -1;
    int64_t price_ticks = 0;
    int64_t available_lots = 0;
    double score = 0.0;
};

const std::vector<QuoteLevel>& levels_for_side(const Order& order, const VenueOrderBookSnapshot& book) {
    return (order.side == SIDE_BUY) ? book.ask_levels : book.bid_levels;
}

bool is_excluded(const std::unordered_set<std::string>& excluded, const std::string& venue_id) {
    return !excluded.empty() && (excluded.find(venue_id) != excluded.end());
}
} // namespace

SmartOrderRouter::SmartOrderRouter(std::vector<VenueCostProfile> profiles) {
    for (const auto& profile : profiles) {
        upsert_profile(profile);
    }
}

void SmartOrderRouter::upsert_profile(const VenueCostProfile& profile) {
    if (profile.venue_id.empty()) return;
    profiles_[profile.venue_id] = profile;
}

bool SmartOrderRouter::get_profile(const std::string& venue_id, VenueCostProfile* out) const {
    if (!out) return false;
    const auto it = profiles_.find(venue_id);
    if (it == profiles_.end()) return false;
    *out = it->second;
    return true;
}

RoutingDecision SmartOrderRouter::route(const Order& order, const std::vector<VenueQuote>& quotes) const {
    std::vector<VenueOrderBookSnapshot> books;
    books.reserve(quotes.size());
    for (const auto& quote : quotes) {
        books.push_back(snapshot_from_top_of_book(quote));
    }
    return route_l2(order, books);
}

RoutingDecision SmartOrderRouter::route_l2(const Order& order, const std::vector<VenueOrderBookSnapshot>& books) const {
    Order normalized = order;
    core::normalize_order_scalars(&normalized);

    RoutingDecision decision{};
    decision.requested_lots = normalized.quantity_lots;
    decision.unrouted_lots = normalized.quantity_lots;

    if (normalized.order_id == 0 || normalized.quantity_lots <= 0) {
        decision.reject_reason = "invalid_order";
        return decision;
    }

    std::vector<RankedLevel> ranked;
    for (const auto& book : books) {
        if (book.venue_id.empty()) continue;

        VenueCostProfile profile{};
        profile.venue_id = book.venue_id;
        auto it = profiles_.find(book.venue_id);
        if (it != profiles_.end()) {
            profile = it->second;
        }

        const auto& levels = levels_for_side(normalized, book);
        for (size_t i = 0; i < levels.size(); ++i) {
            const QuoteLevel& level = levels[i];
            if (!level_is_eligible(normalized, level.price_ticks, level.size_lots)) continue;

            RankedLevel row{};
            row.book = &book;
            row.profile = profile;
            row.level_index = static_cast<int32_t>(i);
            row.price_ticks = level.price_ticks;
            row.available_lots = level.size_lots;
            row.score = effective_cost_score(normalized, level.price_ticks, profile);
            ranked.push_back(row);
        }
    }

    if (ranked.empty()) {
        decision.reject_reason = "no_liquidity";
        return decision;
    }

    std::sort(ranked.begin(), ranked.end(), [&](const RankedLevel& a, const RankedLevel& b) {
        if (a.score == b.score) {
            if (a.price_ticks != b.price_ticks) {
                return (normalized.side == SIDE_BUY)
                    ? (a.price_ticks < b.price_ticks)
                    : (a.price_ticks > b.price_ticks);
            }
            if (a.profile.expected_latency_ms != b.profile.expected_latency_ms) {
                return a.profile.expected_latency_ms < b.profile.expected_latency_ms;
            }
            if (a.book->venue_id != b.book->venue_id) {
                return a.book->venue_id < b.book->venue_id;
            }
            return a.level_index < b.level_index;
        }
        return a.score < b.score;
    });

    int64_t total_available = 0;
    for (const auto& row : ranked) {
        total_available += row.available_lots;
    }

    if (normalized.tif == TIF_FOK && total_available < normalized.quantity_lots) {
        decision.reject_reason = "insufficient_liquidity_fok";
        return decision;
    }

    int64_t remaining = normalized.quantity_lots;
    for (const auto& row : ranked) {
        if (remaining <= 0) break;
        const int64_t take = std::min(remaining, row.available_lots);
        if (take <= 0) continue;

        decision.legs.push_back(RouteLeg{
            row.book->venue_id,
            take,
            row.price_ticks,
            row.level_index,
            row.score
        });

        decision.routed_lots += take;
        remaining -= take;
    }

    decision.unrouted_lots = std::max<int64_t>(0, remaining);
    decision.accepted = (decision.routed_lots > 0);

    if (!decision.accepted) {
        decision.reject_reason = "no_executable_route";
        return decision;
    }

    if (normalized.tif == TIF_FOK && decision.unrouted_lots > 0) {
        decision.accepted = false;
        decision.legs.clear();
        decision.routed_lots = 0;
        decision.unrouted_lots = normalized.quantity_lots;
        decision.reject_reason = "fok_partial_not_allowed";
    }

    return decision;
}

RoutingDecision SmartOrderRouter::reroute_after_partial_fill(
    const Order& original_order,
    int64_t already_executed_lots,
    const std::vector<VenueOrderBookSnapshot>& books,
    const std::vector<std::string>& excluded_venues) const {
    Order normalized = original_order;
    core::normalize_order_scalars(&normalized);

    if (normalized.order_id == 0 || normalized.quantity_lots <= 0) {
        RoutingDecision invalid{};
        invalid.reject_reason = "invalid_order";
        return invalid;
    }

    const int64_t bounded_executed = std::clamp<int64_t>(already_executed_lots, 0, normalized.quantity_lots);
    const int64_t remaining_lots = normalized.quantity_lots - bounded_executed;

    if (remaining_lots <= 0) {
        RoutingDecision done{};
        done.accepted = true;
        done.requested_lots = 0;
        done.routed_lots = 0;
        done.unrouted_lots = 0;
        return done;
    }

    std::unordered_set<std::string> excluded(excluded_venues.begin(), excluded_venues.end());
    std::vector<VenueOrderBookSnapshot> filtered;
    filtered.reserve(books.size());
    for (const auto& book : books) {
        if (is_excluded(excluded, book.venue_id)) continue;
        filtered.push_back(book);
    }

    Order residual = normalized;
    residual.quantity_lots = remaining_lots;
    residual.quantity = core::from_quantity_lots(remaining_lots);

    return route_l2(residual, filtered);
}

bool SmartOrderRouter::level_is_eligible(const Order& order, int64_t price_ticks, int64_t size_lots) {
    if (price_ticks <= 0 || size_lots <= 0) return false;

    if (order.side == SIDE_BUY) {
        if (order.type == ORDER_TYPE_LIMIT && price_ticks > order.price_ticks) return false;
        return true;
    }

    if (order.side == SIDE_SELL) {
        if (order.type == ORDER_TYPE_LIMIT && price_ticks < order.price_ticks) return false;
        return true;
    }

    return false;
}

double SmartOrderRouter::effective_cost_score(const Order& order, int64_t price_ticks, const VenueCostProfile& profile) {
    if (price_ticks <= 0) return std::numeric_limits<double>::infinity();

    const double px = static_cast<double>(price_ticks);
    const double fee_multiplier = profile.taker_fee_bps / 10'000.0;
    const double latency_multiplier =
        (profile.expected_latency_ms * profile.latency_penalty_bps_per_ms) / 10'000.0;

    double adjusted = px;
    if (order.side == SIDE_BUY) {
        adjusted *= (1.0 + fee_multiplier + latency_multiplier);
        return adjusted;
    }

    adjusted *= (1.0 - fee_multiplier - latency_multiplier);
    return -adjusted;
}

} // namespace argentum::gateway
