#pragma once

#include "core/fixed_point.hpp"
#include "core/types.h"
#include "engine/pool_allocator.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace argentum::engine {

class OrderBook {
public:
    static constexpr size_t kMaxPriceLevels = 4096;
    static constexpr size_t kOrderNodePoolSize = 131072;

    explicit OrderBook(const std::string& symbol);
    ~OrderBook() = default;

    OrderBook(const OrderBook&) = delete;
    OrderBook& operator=(const OrderBook&) = delete;

    bool add_order(const Order& order);
    bool cancel_order(uint64_t order_id);
    bool cancel_order_partial(uint64_t order_id, int64_t reduce_lots, Order* out_updated = nullptr);
    bool modify_order(uint64_t order_id, const Order& replacement);
    bool get_order(uint64_t order_id, Order* out_order) const;

    std::vector<Trade> match_order(const Order& incoming, bool rest_residual = true);
    [[nodiscard]] int64_t executable_lots(const Order& incoming) const;

    [[nodiscard]] std::optional<double> get_best_bid() const;
    [[nodiscard]] std::optional<double> get_best_ask() const;
    [[nodiscard]] std::optional<double> get_spread() const;
    [[nodiscard]] std::optional<double> vwap(Side side, double quantity) const;

private:
    struct OrderNode {
        Order order{};
        int32_t prev = -1;
        int32_t next = -1;
    };

    struct PriceLevel {
        int64_t price_ticks = 0;
        int32_t head = -1;
        int32_t tail = -1;
        int64_t total_lots = 0;
        uint32_t order_count = 0;
    };

    struct PriceLevelSet {
        std::array<PriceLevel, kMaxPriceLevels> levels{};
        size_t size = 0;
    };

    struct OrderLocator {
        Side side = SIDE_BUY;
        int64_t price_ticks = 0;
        int32_t node_index = -1;
    };

    static bool is_crossing_level(const Order& incoming, int64_t level_price_ticks);
    static bool is_price_better_or_equal(Side side, int64_t lhs, int64_t rhs);

    [[nodiscard]] PriceLevelSet& levels_for_side(Side side);
    [[nodiscard]] const PriceLevelSet& levels_for_side(Side side) const;
    [[nodiscard]] bool find_level_index(const PriceLevelSet& set, int64_t price_ticks, Side side, size_t* out_index) const;
    [[nodiscard]] PriceLevel* find_or_create_level(PriceLevelSet* set, int64_t price_ticks, Side side);
    void erase_level(PriceLevelSet* set, size_t index);
    void unlink_node(PriceLevel* level, int32_t node_index);
    [[nodiscard]] int32_t allocate_node(const Order& order);
    void release_node(int32_t node_index);
    [[nodiscard]] OrderNode* node_for_order(uint64_t order_id);
    [[nodiscard]] const OrderNode* node_for_order(uint64_t order_id) const;

    std::string symbol_;
    PriceLevelSet bids_{};
    PriceLevelSet asks_{};
    PoolAllocator<OrderNode, kOrderNodePoolSize> order_nodes_{};
    std::unordered_map<uint64_t, OrderLocator> order_lookup_;
    uint64_t next_trade_id_ = 1;
};

} // namespace argentum::engine
