#include "engine/order_book.hpp"

#include "core/order_validator.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace argentum::engine {

namespace {

bool is_valid_normalized_order(const Order& order) {
    return core::validate_order(order) == core::OrderValidationError::None;
}

} // namespace

OrderBook::OrderBook(const std::string& symbol) : symbol_(symbol) {
    order_lookup_.reserve(kOrderNodePoolSize);
}

bool OrderBook::add_order(const Order& order) {
    Order normalized = order;
    core::normalize_order_scalars(&normalized);

    if (!is_valid_normalized_order(normalized)) {
        return false;
    }
    if (order_lookup_.find(normalized.order_id) != order_lookup_.end()) {
        return false;
    }

    const int32_t node_index = allocate_node(normalized);
    if (node_index < 0) {
        return false;
    }

    PriceLevelSet& set = levels_for_side(static_cast<Side>(normalized.side));
    PriceLevel* level = find_or_create_level(&set, normalized.price_ticks, static_cast<Side>(normalized.side));
    if (!level) {
        release_node(node_index);
        return false;
    }

    OrderNode& node = order_nodes_.at(node_index);
    node.prev = level->tail;
    node.next = -1;
    if (level->tail >= 0) {
        order_nodes_.at(level->tail).next = node_index;
    } else {
        level->head = node_index;
    }
    level->tail = node_index;
    level->total_lots += normalized.quantity_lots;
    ++level->order_count;

    auto [_, inserted] = order_lookup_.emplace(
        normalized.order_id,
        OrderLocator{
            static_cast<Side>(normalized.side),
            normalized.price_ticks,
            node_index
        });
    if (!inserted) {
        unlink_node(level, node_index);
        release_node(node_index);

        size_t level_index = 0;
        if (find_level_index(set, normalized.price_ticks, static_cast<Side>(normalized.side), &level_index) &&
            set.levels[level_index].order_count == 0) {
            erase_level(&set, level_index);
        }
        return false;
    }
    return true;
}

bool OrderBook::cancel_order(uint64_t order_id) {
    const auto found = order_lookup_.find(order_id);
    if (found == order_lookup_.end()) {
        return false;
    }

    PriceLevelSet& set = levels_for_side(found->second.side);
    size_t level_index = 0;
    if (!find_level_index(set, found->second.price_ticks, found->second.side, &level_index)) {
        order_lookup_.erase(found);
        return false;
    }

    PriceLevel& level = set.levels[level_index];
    unlink_node(&level, found->second.node_index);
    release_node(found->second.node_index);
    order_lookup_.erase(found);
    if (level.order_count == 0) {
        erase_level(&set, level_index);
    }
    return true;
}

bool OrderBook::cancel_order_partial(uint64_t order_id, int64_t reduce_lots, Order* out_updated) {
    if (reduce_lots <= 0) {
        return false;
    }

    const auto found = order_lookup_.find(order_id);
    if (found == order_lookup_.end()) {
        return false;
    }

    PriceLevelSet& set = levels_for_side(found->second.side);
    size_t level_index = 0;
    if (!find_level_index(set, found->second.price_ticks, found->second.side, &level_index)) {
        return false;
    }

    PriceLevel& level = set.levels[level_index];
    OrderNode& node = order_nodes_.at(found->second.node_index);
    if (reduce_lots >= node.order.quantity_lots) {
        if (out_updated) {
            *out_updated = node.order;
            out_updated->quantity_lots = 0;
            out_updated->quantity = 0.0;
        }

        unlink_node(&level, found->second.node_index);
        release_node(found->second.node_index);
        order_lookup_.erase(found);
        if (level.order_count == 0) {
            erase_level(&set, level_index);
        }
        return true;
    }

    node.order.quantity_lots -= reduce_lots;
    node.order.quantity = core::from_quantity_lots(node.order.quantity_lots);
    level.total_lots -= reduce_lots;
    if (out_updated) {
        *out_updated = node.order;
    }
    return true;
}

bool OrderBook::modify_order(uint64_t order_id, const Order& replacement) {
    Order current{};
    if (!get_order(order_id, &current)) {
        return false;
    }
    if (!cancel_order(order_id)) {
        return false;
    }

    Order normalized = replacement;
    normalized.order_id = order_id;
    core::normalize_order_scalars(&normalized);
    if (!add_order(normalized)) {
        (void)add_order(current);
        return false;
    }
    return true;
}

bool OrderBook::get_order(uint64_t order_id, Order* out_order) const {
    if (!out_order) {
        return false;
    }

    const OrderNode* node = node_for_order(order_id);
    if (!node) {
        return false;
    }

    *out_order = node->order;
    return true;
}

std::vector<Trade> OrderBook::match_order(const Order& incoming, bool rest_residual) {
    std::vector<Trade> trades;
    Order normalized = incoming;
    core::normalize_order_scalars(&normalized);

    if (!is_valid_normalized_order(normalized)) {
        return trades;
    }

    int64_t remaining_lots = normalized.quantity_lots;
    PriceLevelSet& book_side = (normalized.side == SIDE_BUY) ? asks_ : bids_;
    size_t level_index = 0;

    while (level_index < book_side.size && remaining_lots > 0) {
        PriceLevel& level = book_side.levels[level_index];
        if (!is_crossing_level(normalized, level.price_ticks)) {
            break;
        }

        bool level_removed = false;
        int32_t node_index = level.head;
        while (node_index >= 0 && remaining_lots > 0) {
            OrderNode& maker_node = order_nodes_.at(node_index);
            const int32_t next_node = maker_node.next;
            const int64_t fill_lots = std::min(remaining_lots, maker_node.order.quantity_lots);

            Trade trade{};
            trade.trade_id = next_trade_id_++;
            trade.maker_order_id = maker_node.order.order_id;
            trade.taker_order_id = normalized.order_id;
            trade.timestamp_ns = normalized.timestamp_ns;
            trade.price_ticks = level.price_ticks;
            trade.quantity_lots = fill_lots;
            trade.price = core::from_price_ticks(level.price_ticks);
            trade.quantity = core::from_quantity_lots(fill_lots);
            trade.side = normalized.side;
            trades.push_back(trade);

            maker_node.order.quantity_lots -= fill_lots;
            maker_node.order.quantity = core::from_quantity_lots(maker_node.order.quantity_lots);
            level.total_lots -= fill_lots;
            remaining_lots -= fill_lots;

            if (maker_node.order.quantity_lots <= 0) {
                order_lookup_.erase(maker_node.order.order_id);
                unlink_node(&level, node_index);
                release_node(node_index);
                if (level.order_count == 0) {
                    erase_level(&book_side, level_index);
                    level_removed = true;
                    break;
                }
            }

            node_index = next_node;
        }

        if (!level_removed) {
            ++level_index;
        }
    }

    if (rest_residual && remaining_lots > 0 && normalized.type == ORDER_TYPE_LIMIT) {
        Order residual = normalized;
        residual.quantity_lots = remaining_lots;
        residual.quantity = core::from_quantity_lots(remaining_lots);
        (void)add_order(residual);
    }

    return trades;
}

int64_t OrderBook::executable_lots(const Order& incoming) const {
    Order normalized = incoming;
    core::normalize_order_scalars(&normalized);

    if (!is_valid_normalized_order(normalized)) {
        return 0;
    }

    const PriceLevelSet& book_side = (normalized.side == SIDE_BUY) ? asks_ : bids_;
    int64_t remaining_lots = normalized.quantity_lots;
    int64_t filled_lots = 0;

    for (size_t i = 0; i < book_side.size && remaining_lots > 0; ++i) {
        const PriceLevel& level = book_side.levels[i];
        if (!is_crossing_level(normalized, level.price_ticks)) {
            break;
        }

        const int64_t take_lots = std::min(remaining_lots, level.total_lots);
        filled_lots += take_lots;
        remaining_lots -= take_lots;
    }

    return filled_lots;
}

std::optional<double> OrderBook::get_best_bid() const {
    if (bids_.size == 0) {
        return std::nullopt;
    }
    return core::from_price_ticks(bids_.levels[0].price_ticks);
}

std::optional<double> OrderBook::get_best_ask() const {
    if (asks_.size == 0) {
        return std::nullopt;
    }
    return core::from_price_ticks(asks_.levels[0].price_ticks);
}

std::optional<double> OrderBook::get_spread() const {
    const auto bid = get_best_bid();
    const auto ask = get_best_ask();
    if (!bid || !ask) {
        return std::nullopt;
    }
    return *ask - *bid;
}

std::optional<double> OrderBook::vwap(Side side, double quantity) const {
    const int64_t target_lots = core::to_quantity_lots(quantity);
    if (target_lots <= 0 || target_lots == std::numeric_limits<int64_t>::max()) {
        return std::nullopt;
    }

    const PriceLevelSet& book_side = (side == SIDE_BUY) ? asks_ : bids_;
    int64_t remaining_lots = target_lots;
    long double notional_units = 0.0;

    for (size_t i = 0; i < book_side.size && remaining_lots > 0; ++i) {
        const PriceLevel& level = book_side.levels[i];
        const int64_t take_lots = std::min(remaining_lots, level.total_lots);
        notional_units += static_cast<long double>(core::to_notional_units(level.price_ticks, take_lots));
        remaining_lots -= take_lots;
    }

    if (remaining_lots > 0) {
        return std::nullopt;
    }

    const long double avg_ticks = notional_units / static_cast<long double>(target_lots);
    return core::from_price_ticks(static_cast<int64_t>(std::llround(avg_ticks)));
}

bool OrderBook::is_crossing_level(const Order& incoming, int64_t level_price_ticks) {
    if (incoming.type == ORDER_TYPE_LIMIT) {
        if (incoming.side == SIDE_BUY) {
            return level_price_ticks <= incoming.price_ticks;
        }
        return level_price_ticks >= incoming.price_ticks;
    }
    return true;
}

bool OrderBook::is_price_better_or_equal(Side side, int64_t lhs, int64_t rhs) {
    if (side == SIDE_BUY) {
        return lhs >= rhs;
    }
    return lhs <= rhs;
}

OrderBook::PriceLevelSet& OrderBook::levels_for_side(Side side) {
    return (side == SIDE_BUY) ? bids_ : asks_;
}

const OrderBook::PriceLevelSet& OrderBook::levels_for_side(Side side) const {
    return (side == SIDE_BUY) ? bids_ : asks_;
}

bool OrderBook::find_level_index(const PriceLevelSet& set, int64_t price_ticks, Side side, size_t* out_index) const {
    size_t left = 0;
    size_t right = set.size;
    while (left < right) {
        const size_t mid = left + ((right - left) / 2);
        const int64_t mid_price = set.levels[mid].price_ticks;
        if (mid_price == price_ticks) {
            if (out_index) {
                *out_index = mid;
            }
            return true;
        }

        if (is_price_better_or_equal(side, mid_price, price_ticks)) {
            left = mid + 1;
        } else {
            right = mid;
        }
    }

    if (out_index) {
        *out_index = left;
    }
    return false;
}

OrderBook::PriceLevel* OrderBook::find_or_create_level(PriceLevelSet* set, int64_t price_ticks, Side side) {
    if (!set) {
        return nullptr;
    }

    size_t level_index = 0;
    if (find_level_index(*set, price_ticks, side, &level_index)) {
        return &set->levels[level_index];
    }
    if (set->size >= kMaxPriceLevels) {
        return nullptr;
    }

    for (size_t i = set->size; i > level_index; --i) {
        set->levels[i] = set->levels[i - 1];
    }
    set->levels[level_index] = PriceLevel{
        price_ticks,
        -1,
        -1,
        0,
        0
    };
    ++set->size;
    return &set->levels[level_index];
}

void OrderBook::erase_level(PriceLevelSet* set, size_t index) {
    if (!set || index >= set->size) {
        return;
    }

    for (size_t i = index + 1; i < set->size; ++i) {
        set->levels[i - 1] = set->levels[i];
    }
    set->levels[set->size - 1] = PriceLevel{};
    --set->size;
}

void OrderBook::unlink_node(PriceLevel* level, int32_t node_index) {
    if (!level || !order_nodes_.in_use(node_index)) {
        return;
    }

    OrderNode& node = order_nodes_.at(node_index);
    if (node.prev >= 0) {
        order_nodes_.at(node.prev).next = node.next;
    } else {
        level->head = node.next;
    }

    if (node.next >= 0) {
        order_nodes_.at(node.next).prev = node.prev;
    } else {
        level->tail = node.prev;
    }

    level->total_lots -= node.order.quantity_lots;
    if (level->order_count > 0) {
        --level->order_count;
    }
}

int32_t OrderBook::allocate_node(const Order& order) {
    return order_nodes_.try_emplace(OrderNode{order, -1, -1});
}

void OrderBook::release_node(int32_t node_index) {
    order_nodes_.deallocate(node_index);
}

OrderBook::OrderNode* OrderBook::node_for_order(uint64_t order_id) {
    const auto found = order_lookup_.find(order_id);
    if (found == order_lookup_.end() || !order_nodes_.in_use(found->second.node_index)) {
        return nullptr;
    }
    return &order_nodes_.at(found->second.node_index);
}

const OrderBook::OrderNode* OrderBook::node_for_order(uint64_t order_id) const {
    const auto found = order_lookup_.find(order_id);
    if (found == order_lookup_.end() || !order_nodes_.in_use(found->second.node_index)) {
        return nullptr;
    }
    return &order_nodes_.at(found->second.node_index);
}

} // namespace argentum::engine
