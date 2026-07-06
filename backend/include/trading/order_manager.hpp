#pragma once

#include "core/types.h"
#include "risk/risk_manager.hpp"
#include "engine/order_book.hpp"
#include "core/fixed_point.hpp"
#include "core/time_utils.hpp"
#include <deque>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace argentum::persist {
class AsyncEventJournal;
using EventJournal = AsyncEventJournal;
struct JournalEvent;
enum class JournalEventType : uint8_t;
}

namespace argentum::trading {

enum class OrderRejectReason {
    None = 0,
    InvalidOrder = 1,
    DuplicateOrderId = 2,
    RiskRejected = 3,
    InternalError = 4,
    LiquidityUnavailable = 5
};

enum class OrderStatus {
    New = 0,
    Resting = 1,
    PartiallyFilled = 2,
    Filled = 3,
    Canceled = 4,
    Rejected = 5
};

struct OrderState {
    Order order{};
    int64_t initial_lots = 0;
    int64_t remaining_lots = 0;
    int64_t filled_lots = 0;
    OrderStatus status = OrderStatus::New;
    OrderRejectReason reject_reason = OrderRejectReason::None;
    uint64_t updated_at_ns = 0;
};

struct OrderSubmissionResult {
    bool accepted = false;
    bool resting = false;
    double filled_quantity = 0.0;
    double remaining_quantity = 0.0;
    OrderStatus status = OrderStatus::New;
    OrderRejectReason reject_reason = OrderRejectReason::None;
    std::vector<Trade> trades;
};

/**
 * @class OrderManager
 * @brief Orchestrates the lifecycle of orders.
 */
class OrderManager {
public:
    OrderManager(std::shared_ptr<risk::RiskManager> risk, 
                 std::shared_ptr<engine::OrderBook> book,
                 std::shared_ptr<persist::EventJournal> journal = nullptr);

    /**
     * @brief Entry point for new orders from API/Strategy.
     * @param related_signal_id Optional id of the signal_decision audit event
     *        that produced this order; journaled for replay correlation.
     */
    OrderSubmissionResult submit_order(const Order& order, uint64_t related_signal_id = 0);

    bool cancel_order(uint64_t order_id);
    bool cancel_order_partial(uint64_t order_id, double quantity);
    bool modify_order(uint64_t order_id, double new_price, double new_quantity);
    bool get_order_state(uint64_t order_id, OrderState* out_state) const;
    size_t active_order_count() const;
    size_t history_order_count() const;
    void set_history_cap(size_t n);

private:
    // Internal helpers; caller must hold mutex_.
    void upsert_state(const OrderState& state);
    void enforce_history_cap_unlocked();
    void apply_trade_to_maker(uint64_t maker_order_id, const Trade& trade);
    void emit_event_unlocked(persist::JournalEvent&& event);

    struct HistoryEntry {
        uint64_t order_id = 0;
        uint64_t updated_at_ns = 0;
    };

    std::shared_ptr<risk::RiskManager> risk_manager_;
    std::shared_ptr<engine::OrderBook> order_book_;
    std::shared_ptr<persist::EventJournal> journal_;
    mutable std::mutex mutex_;
    std::unordered_map<uint64_t, OrderState> active_orders_;
    std::unordered_map<uint64_t, OrderState> order_history_;
    std::deque<HistoryEntry> history_order_queue_;
    size_t history_cap_ = 1'000'000;
};

} // namespace argentum::trading
