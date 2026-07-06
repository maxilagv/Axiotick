#include "trading/order_manager.hpp"

#include "audit/logger.hpp"
#include "core/order_validator.hpp"
#include "persist/event_journal.hpp"

#include <algorithm>

namespace argentum::trading {

OrderManager::OrderManager(std::shared_ptr<risk::RiskManager> risk, 
                           std::shared_ptr<engine::OrderBook> book,
                           std::shared_ptr<persist::EventJournal> journal)
    : risk_manager_(std::move(risk)),
      order_book_(std::move(book)),
      journal_(std::move(journal)) {}

OrderSubmissionResult OrderManager::submit_order(const Order& order, uint64_t related_signal_id) {
    OrderSubmissionResult result{};
    Order normalized = order;
    core::normalize_order_scalars(&normalized);
    result.remaining_quantity = normalized.quantity;

    if (!risk_manager_ || !order_book_) {
        result.reject_reason = OrderRejectReason::InternalError;
        result.status = OrderStatus::Rejected;
        return result;
    }

    if (!core::is_valid_order(normalized)) {
        result.reject_reason = OrderRejectReason::InvalidOrder;
        result.status = OrderStatus::Rejected;
        emit_event_unlocked(persist::JournalEvent{
            .timestamp_ns = core::unix_now_ns(),
            .type = persist::JournalEventType::OrderRejected,
            .order_id = normalized.order_id,
            .price_ticks = normalized.price_ticks,
            .quantity_lots = normalized.quantity_lots,
            .remaining_lots = normalized.quantity_lots,
            .reason_code = static_cast<int32_t>(result.reject_reason),
            .side = normalized.side,
            .order_type = normalized.type,
            .tif = normalized.tif,
            .related_signal_id = related_signal_id
        });
        return result;
    }

    // Serialize the full OMS + order book mutation path.
    std::lock_guard<std::mutex> lock(mutex_);
    if (active_orders_.find(normalized.order_id) != active_orders_.end() ||
        order_history_.find(normalized.order_id) != order_history_.end()) {
        result.reject_reason = OrderRejectReason::DuplicateOrderId;
        result.status = OrderStatus::Rejected;
        emit_event_unlocked(persist::JournalEvent{
            .timestamp_ns = core::unix_now_ns(),
            .type = persist::JournalEventType::OrderRejected,
            .order_id = normalized.order_id,
            .price_ticks = normalized.price_ticks,
            .quantity_lots = normalized.quantity_lots,
            .remaining_lots = normalized.quantity_lots,
            .reason_code = static_cast<int32_t>(result.reject_reason),
            .side = normalized.side,
            .order_type = normalized.type,
            .tif = normalized.tif,
            .related_signal_id = related_signal_id
        });
        return result;
    }

    if (normalized.tif == TIF_FOK) {
        const int64_t immediate_lots = order_book_->executable_lots(normalized);
        if (immediate_lots < normalized.quantity_lots) {
            result.reject_reason = OrderRejectReason::LiquidityUnavailable;
            result.status = OrderStatus::Rejected;
            OrderState rejected{};
            rejected.order = normalized;
            rejected.initial_lots = normalized.quantity_lots;
            rejected.remaining_lots = normalized.quantity_lots;
            rejected.status = OrderStatus::Rejected;
            rejected.reject_reason = result.reject_reason;
            rejected.updated_at_ns = core::unix_now_ns();
            upsert_state(rejected);
            emit_event_unlocked(persist::JournalEvent{
                .timestamp_ns = rejected.updated_at_ns,
                .type = persist::JournalEventType::OrderRejected,
                .order_id = normalized.order_id,
                .price_ticks = normalized.price_ticks,
                .quantity_lots = normalized.quantity_lots,
                .remaining_lots = normalized.quantity_lots,
                .reason_code = static_cast<int32_t>(result.reject_reason),
                .side = normalized.side,
                .order_type = normalized.type,
                .tif = normalized.tif,
                .related_signal_id = related_signal_id
            });
            return result;
        }
    }

    if (!risk_manager_->check_order(normalized)) {
        result.reject_reason = OrderRejectReason::RiskRejected;
        result.status = OrderStatus::Rejected;
        OrderState rejected{};
        rejected.order = normalized;
        rejected.initial_lots = normalized.quantity_lots;
        rejected.remaining_lots = normalized.quantity_lots;
        rejected.status = OrderStatus::Rejected;
        rejected.reject_reason = result.reject_reason;
        rejected.updated_at_ns = core::unix_now_ns();
        upsert_state(rejected);
        emit_event_unlocked(persist::JournalEvent{
            .timestamp_ns = rejected.updated_at_ns,
            .type = persist::JournalEventType::OrderRejected,
            .order_id = normalized.order_id,
            .price_ticks = normalized.price_ticks,
            .quantity_lots = normalized.quantity_lots,
            .remaining_lots = normalized.quantity_lots,
            .reason_code = static_cast<int32_t>(result.reject_reason),
            .side = normalized.side,
            .order_type = normalized.type,
            .tif = normalized.tif,
            .related_signal_id = related_signal_id
        });
        ARGENTUM_LOG(WARN, "[OMS] order rejected by risk order_id=" << normalized.order_id);
        return result;
    }

    OrderState taker_state{};
    taker_state.order = normalized;
    taker_state.initial_lots = normalized.quantity_lots;
    taker_state.remaining_lots = normalized.quantity_lots;
    taker_state.updated_at_ns = core::unix_now_ns();
    taker_state.status = OrderStatus::New;

    const bool rest_residual = (normalized.type == ORDER_TYPE_LIMIT && normalized.tif == TIF_GTC);
    result.trades = order_book_->match_order(normalized, rest_residual);
    for (const Trade& trade : result.trades) {
        Order taker_fill = normalized;
        taker_fill.price = trade.price;
        taker_fill.quantity = trade.quantity;
        taker_fill.price_ticks = trade.price_ticks;
        taker_fill.quantity_lots = trade.quantity_lots;
        risk_manager_->on_fill(taker_fill);
        emit_event_unlocked(persist::JournalEvent{
            .timestamp_ns = trade.timestamp_ns,
            .type = persist::JournalEventType::TradeExecuted,
            .order_id = trade.taker_order_id,
            .related_order_id = trade.maker_order_id,
            .price_ticks = trade.price_ticks,
            .quantity_lots = trade.quantity_lots,
            .remaining_lots = 0,
            .reason_code = 0,
            .side = taker_fill.side,
            .order_type = taker_fill.type,
            .tif = taker_fill.tif,
            .related_signal_id = related_signal_id
        });
        result.filled_quantity += trade.quantity;
        taker_state.filled_lots += trade.quantity_lots;
        taker_state.remaining_lots = std::max<int64_t>(0, taker_state.remaining_lots - trade.quantity_lots);
        apply_trade_to_maker(trade.maker_order_id, trade);
    }

    result.remaining_quantity = core::from_quantity_lots(taker_state.remaining_lots);
    Order resting_order{};
    result.resting = (rest_residual && taker_state.remaining_lots > 0 &&
                      order_book_->get_order(normalized.order_id, &resting_order));

    if (result.resting) {
        Order residual = normalized;
        residual.quantity_lots = taker_state.remaining_lots;
        residual.quantity = core::from_quantity_lots(taker_state.remaining_lots);
        taker_state.order = residual;
        taker_state.status = (taker_state.filled_lots > 0) ? OrderStatus::PartiallyFilled : OrderStatus::Resting;
        taker_state.updated_at_ns = core::unix_now_ns();
        active_orders_[residual.order_id] = taker_state;
        upsert_state(taker_state);
        emit_event_unlocked(persist::JournalEvent{
            .timestamp_ns = taker_state.updated_at_ns,
            .type = persist::JournalEventType::OrderAccepted,
            .order_id = residual.order_id,
            .price_ticks = residual.price_ticks,
            .quantity_lots = taker_state.initial_lots,
            .remaining_lots = taker_state.remaining_lots,
            .reason_code = 0,
            .side = residual.side,
            .order_type = residual.type,
            .tif = residual.tif,
            .resting = true,
            .related_signal_id = related_signal_id
        });
        ARGENTUM_LOG(
            INFO,
            "[OMS] order accepted order_id=" << normalized.order_id
            << " remaining_quantity=" << result.remaining_quantity);
    } else {
        if (taker_state.remaining_lots > 0) {
            Order canceled = normalized;
            canceled.quantity_lots = taker_state.remaining_lots;
            canceled.quantity = result.remaining_quantity;
            risk_manager_->on_cancel(canceled);
        }
        taker_state.status = (taker_state.remaining_lots == 0)
            ? (taker_state.filled_lots > 0 ? OrderStatus::Filled : OrderStatus::Canceled)
            : (taker_state.filled_lots > 0 ? OrderStatus::PartiallyFilled : OrderStatus::Canceled);
        taker_state.remaining_lots = 0;
        taker_state.order.quantity_lots = 0;
        taker_state.order.quantity = 0.0;
        taker_state.updated_at_ns = core::unix_now_ns();
        upsert_state(taker_state);
        emit_event_unlocked(persist::JournalEvent{
            .timestamp_ns = taker_state.updated_at_ns,
            .type = persist::JournalEventType::OrderAccepted,
            .order_id = normalized.order_id,
            .price_ticks = normalized.price_ticks,
            .quantity_lots = taker_state.initial_lots,
            .remaining_lots = 0,
            .reason_code = 0,
            .side = normalized.side,
            .order_type = normalized.type,
            .tif = normalized.tif,
            .resting = false,
            .related_signal_id = related_signal_id
        });
        ARGENTUM_LOG(INFO, "[OMS] order fully processed order_id=" << normalized.order_id);
    }

    result.accepted = true;
    result.status = taker_state.status;
    return result;
}

bool OrderManager::cancel_order(uint64_t order_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = active_orders_.find(order_id);
    if (it == active_orders_.end()) return false;

    if (!order_book_->cancel_order(order_id)) {
        return false;
    }

    OrderState state = it->second;
    risk_manager_->on_cancel(state.order);
    state.status = OrderStatus::Canceled;
    state.order.quantity_lots = 0;
    state.order.quantity = 0.0;
    state.remaining_lots = 0;
    state.updated_at_ns = core::unix_now_ns();
    active_orders_.erase(it);
    upsert_state(state);
    emit_event_unlocked(persist::JournalEvent{
        .timestamp_ns = state.updated_at_ns,
        .type = persist::JournalEventType::OrderCanceled,
        .order_id = order_id,
        .price_ticks = state.order.price_ticks,
        .quantity_lots = state.initial_lots,
        .remaining_lots = 0,
        .reason_code = 0,
        .side = state.order.side,
        .order_type = state.order.type,
        .tif = state.order.tif,
        .resting = false
    });
    ARGENTUM_LOG(INFO, "[OMS] order canceled order_id=" << order_id);
    return true;
}

bool OrderManager::cancel_order_partial(uint64_t order_id, double quantity) {
    const int64_t reduce_lots = core::to_quantity_lots(quantity);
    if (reduce_lots <= 0) return false;

    std::lock_guard<std::mutex> lock(mutex_);
    auto it = active_orders_.find(order_id);
    if (it == active_orders_.end()) return false;

    Order updated{};
    if (!order_book_->cancel_order_partial(order_id, reduce_lots, &updated)) {
        return false;
    }

    if (updated.quantity_lots <= 0) {
        risk_manager_->on_cancel(it->second.order);
        OrderState state = it->second;
        state.status = OrderStatus::Canceled;
        state.remaining_lots = 0;
        state.order.quantity_lots = 0;
        state.order.quantity = 0.0;
        state.updated_at_ns = core::unix_now_ns();
        active_orders_.erase(it);
        upsert_state(state);
        emit_event_unlocked(persist::JournalEvent{
            .timestamp_ns = state.updated_at_ns,
            .type = persist::JournalEventType::OrderCanceled,
            .order_id = order_id,
            .price_ticks = state.order.price_ticks,
            .quantity_lots = state.initial_lots,
            .remaining_lots = 0,
            .reason_code = 0,
            .side = state.order.side,
            .order_type = state.order.type,
            .tif = state.order.tif,
            .resting = false
        });
        return true;
    }

    OrderState& state = it->second;
    const int64_t old_remaining = state.remaining_lots;
    state.order = updated;
    state.remaining_lots = updated.quantity_lots;
    state.filled_lots = state.initial_lots - state.remaining_lots;
    state.status = (state.filled_lots > 0) ? OrderStatus::PartiallyFilled : OrderStatus::Resting;
    state.updated_at_ns = core::unix_now_ns();

    const int64_t released = std::max<int64_t>(0, old_remaining - state.remaining_lots);
    if (released > 0) {
        Order canceled = updated;
        canceled.quantity_lots = released;
        canceled.quantity = core::from_quantity_lots(released);
        risk_manager_->on_cancel(canceled);
    }
    upsert_state(state);
    emit_event_unlocked(persist::JournalEvent{
        .timestamp_ns = state.updated_at_ns,
        .type = persist::JournalEventType::OrderReplaced,
        .order_id = order_id,
        .price_ticks = state.order.price_ticks,
        .quantity_lots = state.initial_lots,
        .remaining_lots = state.remaining_lots,
        .reason_code = 0,
        .side = state.order.side,
        .order_type = state.order.type,
        .tif = state.order.tif,
        .resting = true
    });
    return true;
}

bool OrderManager::modify_order(uint64_t order_id, double new_price, double new_quantity) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = active_orders_.find(order_id);
    if (it == active_orders_.end()) return false;

    Order replacement = it->second.order;
    replacement.price = new_price;
    replacement.quantity = new_quantity;
    replacement.price_ticks = core::to_price_ticks(new_price);
    replacement.quantity_lots = core::to_quantity_lots(new_quantity);
    core::normalize_order_scalars(&replacement);

    if (!core::is_valid_order(replacement)) return false;
    if (!order_book_->modify_order(order_id, replacement)) return false;

    // Rebuild risk reservation using delta between old and new remaining.
    risk_manager_->on_cancel(it->second.order);
    if (!risk_manager_->check_order(replacement)) {
        (void)order_book_->modify_order(order_id, it->second.order);
        (void)risk_manager_->check_order(it->second.order);
        return false;
    }

    OrderState& state = it->second;
    state.order = replacement;
    state.initial_lots = replacement.quantity_lots;
    state.remaining_lots = replacement.quantity_lots;
    state.filled_lots = 0;
    state.status = OrderStatus::Resting;
    state.updated_at_ns = core::unix_now_ns();
    upsert_state(state);
    emit_event_unlocked(persist::JournalEvent{
        .timestamp_ns = state.updated_at_ns,
        .type = persist::JournalEventType::OrderReplaced,
        .order_id = order_id,
        .price_ticks = state.order.price_ticks,
        .quantity_lots = state.initial_lots,
        .remaining_lots = state.remaining_lots,
        .reason_code = 0,
        .side = state.order.side,
        .order_type = state.order.type,
        .tif = state.order.tif,
        .resting = true
    });
    return true;
}

bool OrderManager::get_order_state(uint64_t order_id, OrderState* out_state) const {
    if (!out_state) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    auto active_it = active_orders_.find(order_id);
    if (active_it != active_orders_.end()) {
        *out_state = active_it->second;
        return true;
    }
    auto hist_it = order_history_.find(order_id);
    if (hist_it == order_history_.end()) return false;
    *out_state = hist_it->second;
    return true;
}

size_t OrderManager::active_order_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return active_orders_.size();
}

size_t OrderManager::history_order_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return order_history_.size();
}

void OrderManager::set_history_cap(size_t n) {
    std::lock_guard<std::mutex> lock(mutex_);
    history_cap_ = std::max<size_t>(1, n);
    enforce_history_cap_unlocked();
}

void OrderManager::upsert_state(const OrderState& state) {
    // Caller must hold mutex_.
    order_history_[state.order.order_id] = state;
    history_order_queue_.push_back(HistoryEntry{state.order.order_id, state.updated_at_ns});
    enforce_history_cap_unlocked();
}

void OrderManager::enforce_history_cap_unlocked() {
    while (order_history_.size() > history_cap_ && !history_order_queue_.empty()) {
        const HistoryEntry entry = history_order_queue_.front();
        history_order_queue_.pop_front();

        const auto it = order_history_.find(entry.order_id);
        if (it == order_history_.end()) {
            continue;
        }
        if (it->second.updated_at_ns != entry.updated_at_ns) {
            continue;
        }
        if (active_orders_.find(entry.order_id) != active_orders_.end()) {
            continue;
        }
        order_history_.erase(it);
    }
}

void OrderManager::apply_trade_to_maker(uint64_t maker_order_id, const Trade& trade) {
    // Caller must hold mutex_.
    auto maker_it = active_orders_.find(maker_order_id);
    if (maker_it == active_orders_.end()) {
        return;
    }

    OrderState& maker = maker_it->second;
    Order maker_fill = maker.order;
    maker_fill.price_ticks = trade.price_ticks;
    maker_fill.quantity_lots = trade.quantity_lots;
    maker_fill.price = trade.price;
    maker_fill.quantity = trade.quantity;
    risk_manager_->on_fill(maker_fill);

    maker.filled_lots += trade.quantity_lots;
    maker.remaining_lots = std::max<int64_t>(0, maker.remaining_lots - trade.quantity_lots);
    maker.order.quantity_lots = maker.remaining_lots;
    maker.order.quantity = core::from_quantity_lots(maker.remaining_lots);
    maker.status = (maker.remaining_lots == 0) ? OrderStatus::Filled : OrderStatus::PartiallyFilled;
    maker.updated_at_ns = core::unix_now_ns();

    if (maker.remaining_lots == 0) {
        upsert_state(maker);
        active_orders_.erase(maker_it);
        return;
    }
    upsert_state(maker);
}

void OrderManager::emit_event_unlocked(persist::JournalEvent&& event) {
    if (!journal_) return;
    if (event.timestamp_ns == 0) {
        event.timestamp_ns = core::unix_now_ns();
    }
    (void)journal_->append(event);
}

} // namespace argentum::trading
