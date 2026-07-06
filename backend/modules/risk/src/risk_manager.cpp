#include "risk/risk_manager.hpp"

#include "audit/logger.hpp"
#include "core/order_validator.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <limits>

namespace argentum::risk {

RiskManager::RiskManager(RiskLimits limits) : limits_(std::move(limits)) {
    reservations_.reserve(65536);

    for (const auto& limit : limits_.symbol_limits) {
        const std::string key = normalize_symbol(limit.symbol.c_str());
        if (key.empty()) {
            continue;
        }

        auto state = std::make_unique<SymbolLimitState>();
        state->limit = limit;
        symbol_limit_states_[key] = std::move(state);
    }
}

bool RiskManager::check_order(const Order& order) {
    if (kill_switch_active_.load(std::memory_order_acquire)) {
        ARGENTUM_LOG(WARN, "[Risk] reject kill switch active order_id=" << order.order_id);
        return false;
    }

    Order normalized = order;
    core::normalize_order_scalars(&normalized);

    const auto validation = core::validate_order(normalized);
    if (validation != core::OrderValidationError::None) {
        ARGENTUM_LOG(WARN, "[Risk] reject invalid order fields order_id=" << normalized.order_id);
        return false;
    }

    const double order_value_abs = std::abs(normalized.price * normalized.quantity);
    if (order_value_abs > limits_.max_order_value) {
        ARGENTUM_LOG(
            WARN,
            "[Risk] reject order value order_id=" << normalized.order_id
            << " order_value=" << order_value_abs
            << " max_order_value=" << limits_.max_order_value);
        return false;
    }

    const int64_t delta = signed_notional_units(normalized);
    const int64_t delta_lots = signed_position_lots(normalized);
    const std::string symbol_key = normalize_symbol(normalized.symbol);

    if (!symbol_key.empty()) {
        const auto limit_it = symbol_limit_states_.find(symbol_key);
        if (limit_it != symbol_limit_states_.end()) {
            const int64_t current_net_lots = limit_it->second->net_position_lots.load(std::memory_order_acquire);
            const int64_t proposed_net_lots = current_net_lots + delta_lots;

            if (limit_it->second->limit.max_net_position > 0.0) {
                const int64_t max_net_lots = core::to_quantity_lots(limit_it->second->limit.max_net_position);
                if (max_net_lots == std::numeric_limits<int64_t>::max() ||
                    std::llabs(proposed_net_lots) > std::llabs(max_net_lots)) {
                    ARGENTUM_LOG(
                        WARN,
                        "[Risk] reject symbol net limit order_id=" << normalized.order_id
                        << " symbol=" << symbol_key
                        << " proposed_net_lots=" << proposed_net_lots
                        << " max_net_lots=" << max_net_lots);
                    return false;
                }
            }

            if (limit_it->second->limit.max_gross_position > 0.0) {
                const int64_t max_gross_lots = core::to_quantity_lots(limit_it->second->limit.max_gross_position);
                if (max_gross_lots == std::numeric_limits<int64_t>::max() ||
                    std::llabs(proposed_net_lots) > std::llabs(max_gross_lots)) {
                    ARGENTUM_LOG(
                        WARN,
                        "[Risk] reject symbol gross limit order_id=" << normalized.order_id
                        << " symbol=" << symbol_key
                        << " proposed_gross_lots=" << std::llabs(proposed_net_lots)
                        << " max_gross_lots=" << max_gross_lots);
                    return false;
                }
            }
        }
    }

    std::lock_guard<std::mutex> lock(reservations_mutex_);
    if (reservations_.find(normalized.order_id) != reservations_.end()) {
        ARGENTUM_LOG(WARN, "[Risk] reject duplicate reservation order_id=" << normalized.order_id);
        return false;
    }

    int64_t current = committed_exposure_units_.load(std::memory_order_acquire);
    for (;;) {
        int64_t proposed = 0;
        if (!try_add_units(current, delta, &proposed)) {
            ARGENTUM_LOG(WARN, "[Risk] reject exposure overflow order_id=" << normalized.order_id);
            return false;
        }

        const double proposed_abs = std::abs(static_cast<double>(proposed)) /
                                    static_cast<double>(core::kNotionalScale);
        if (proposed_abs > limits_.max_position_exposure) {
            ARGENTUM_LOG(
                WARN,
                "[Risk] reject global exposure order_id=" << normalized.order_id
                << " proposed=" << proposed_abs
                << " max_position_exposure=" << limits_.max_position_exposure);
            return false;
        }

        if (committed_exposure_units_.compare_exchange_weak(
                current,
                proposed,
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            break;
        }
    }

    reservations_[normalized.order_id] = Reservation{
        static_cast<Side>(normalized.side),
        normalized.price_ticks,
        normalized.quantity_lots,
        symbol_key
    };
    return true;
}

void RiskManager::on_fill(const Order& order) {
    Order normalized = order;
    core::normalize_order_scalars(&normalized);
    if (core::validate_order(normalized) != core::OrderValidationError::None) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(reservations_mutex_);
        auto it = reservations_.find(normalized.order_id);
        if (it != reservations_.end()) {
            const int64_t release_lots = std::min(normalized.quantity_lots, it->second.remaining_lots);
            if (release_lots > 0) {
                int64_t release_units = core::to_notional_units(it->second.reserved_price_ticks, release_lots);
                if (it->second.side == SIDE_SELL) {
                    release_units = -release_units;
                }
                committed_exposure_units_.fetch_sub(release_units, std::memory_order_acq_rel);
                it->second.remaining_lots -= release_lots;
            }
            if (it->second.remaining_lots <= 0) {
                reservations_.erase(it);
            }
        }
    }

    const int64_t fill_units = signed_notional_units(normalized);
    filled_exposure_units_.fetch_add(fill_units, std::memory_order_acq_rel);

    const int64_t delta_lots = signed_position_lots(normalized);
    const std::string symbol_key = normalize_symbol(normalized.symbol);
    if (!symbol_key.empty()) {
        const auto limit_it = symbol_limit_states_.find(symbol_key);
        if (limit_it != symbol_limit_states_.end()) {
            limit_it->second->net_position_lots.fetch_add(delta_lots, std::memory_order_acq_rel);
        }

        std::lock_guard<std::mutex> lock(symbol_positions_mutex_);
        update_position_on_fill_locked(
            symbol_key,
            normalized.side,
            core::from_price_ticks(normalized.price_ticks),
            normalized.quantity_lots);
        check_daily_loss_locked();
    }
}

void RiskManager::update_position_on_fill_locked(
    const std::string& symbol_key,
    uint8_t side,
    double price,
    int64_t lots) {
    if (lots <= 0 || !(price > 0.0) || !std::isfinite(price)) {
        return;
    }

    PositionState& position = positions_[symbol_key];
    const int64_t delta_lots = (side == SIDE_BUY) ? lots : -lots;
    const double fill_qty = core::from_quantity_lots(lots);

    if (position.net_lots == 0 || (position.net_lots > 0) == (delta_lots > 0)) {
        // Opening or extending: fold the fill into the average entry price.
        const double old_qty = std::abs(core::from_quantity_lots(position.net_lots));
        const double new_qty = old_qty + fill_qty;
        if (new_qty > 0.0) {
            position.avg_price = (position.avg_price * old_qty + price * fill_qty) / new_qty;
        }
        position.net_lots += delta_lots;
    } else {
        // Reducing, closing or flipping: realize PnL on the closed portion.
        const int64_t closed_lots = std::min<int64_t>(std::llabs(position.net_lots), lots);
        const double closed_qty = core::from_quantity_lots(closed_lots);
        const double direction = (position.net_lots > 0) ? 1.0 : -1.0;
        realized_pnl_total_ += (price - position.avg_price) * closed_qty * direction;

        const bool was_long = position.net_lots > 0;
        position.net_lots += delta_lots;
        if (position.net_lots == 0) {
            position.avg_price = 0.0;
        } else if ((position.net_lots > 0) != was_long) {
            // Flipped through zero: the residual opens at the fill price.
            position.avg_price = price;
        }
    }

    position.last_mark_price = price;
    refresh_unrealized_locked(position);
}

void RiskManager::refresh_unrealized_locked(PositionState& position) {
    const double old_unrealized = position.unrealized_pnl;
    double unrealized = 0.0;
    if (position.net_lots != 0 && position.last_mark_price > 0.0) {
        unrealized = (position.last_mark_price - position.avg_price) *
                     core::from_quantity_lots(position.net_lots);
    }
    position.unrealized_pnl = unrealized;
    unrealized_pnl_total_ += (unrealized - old_unrealized);
}

double RiskManager::daily_pnl_locked() const {
    return realized_pnl_total_ + unrealized_pnl_total_ - day_baseline_pnl_;
}

void RiskManager::check_daily_loss_locked() {
    if (limits_.max_daily_loss <= 0.0) {
        return;
    }
    if (kill_switch_active_.load(std::memory_order_acquire)) {
        return;
    }

    const double pnl = daily_pnl_locked();
    if (pnl <= -limits_.max_daily_loss) {
        char reason[160];
        std::snprintf(
            reason,
            sizeof(reason),
            "daily_loss_limit_breached daily_pnl=%.2f max_daily_loss=%.2f",
            pnl,
            limits_.max_daily_loss);
        trigger_kill_switch(reason);
    }
}

void RiskManager::mark_to_market(const std::string& symbol, double price) {
    if (!(price > 0.0) || !std::isfinite(price)) {
        return;
    }
    const std::string key = normalize_symbol(symbol.c_str());
    if (key.empty()) {
        return;
    }

    std::lock_guard<std::mutex> lock(symbol_positions_mutex_);
    const auto it = positions_.find(key);
    if (it == positions_.end()) {
        return;
    }
    it->second.last_mark_price = price;
    refresh_unrealized_locked(it->second);
    check_daily_loss_locked();
}

void RiskManager::maybe_roll_day(uint64_t now_ns) {
    constexpr uint64_t kNsPerDay = 86'400ULL * 1'000'000'000ULL;
    const uint64_t day_index = now_ns / kNsPerDay;

    std::lock_guard<std::mutex> lock(symbol_positions_mutex_);
    if (current_day_index_ == 0) {
        current_day_index_ = day_index;
        return;
    }
    if (day_index == current_day_index_) {
        return;
    }

    current_day_index_ = day_index;
    day_baseline_pnl_ = realized_pnl_total_ + unrealized_pnl_total_;

    char baseline[64];
    std::snprintf(baseline, sizeof(baseline), "%.2f", day_baseline_pnl_);
    char day_buf[32];
    std::snprintf(day_buf, sizeof(day_buf), "%llu", static_cast<unsigned long long>(day_index));
    audit::Logger::instance().structured_log(
        audit::LogLevel::AUDIT,
        "risk_day_rolled",
        {{"utc_day_index", day_buf}, {"pnl_baseline", baseline}});
}

double RiskManager::daily_pnl() const {
    std::lock_guard<std::mutex> lock(symbol_positions_mutex_);
    return daily_pnl_locked();
}

double RiskManager::realized_pnl() const {
    std::lock_guard<std::mutex> lock(symbol_positions_mutex_);
    return realized_pnl_total_;
}

double RiskManager::unrealized_pnl() const {
    std::lock_guard<std::mutex> lock(symbol_positions_mutex_);
    return unrealized_pnl_total_;
}

bool RiskManager::kill_switch_active() const {
    return kill_switch_active_.load(std::memory_order_acquire);
}

void RiskManager::trigger_kill_switch(const std::string& reason) {
    bool expected = false;
    if (!kill_switch_active_.compare_exchange_strong(
            expected,
            true,
            std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        return;
    }

    audit::Logger::instance().structured_log(
        audit::LogLevel::AUDIT,
        "risk_kill_switch_triggered",
        {{"reason", reason}});
    ARGENTUM_LOG(CRITICAL, "[Risk] kill switch triggered reason=" << reason);
}

void RiskManager::reset_kill_switch(const std::string& reason) {
    bool expected = true;
    if (!kill_switch_active_.compare_exchange_strong(
            expected,
            false,
            std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        return;
    }

    audit::Logger::instance().structured_log(
        audit::LogLevel::AUDIT,
        "risk_kill_switch_reset",
        {{"reason", reason}});
    ARGENTUM_LOG(WARN, "[Risk] kill switch reset reason=" << reason);
}

void RiskManager::on_cancel(const Order& order) {
    Order normalized = order;
    core::normalize_order_scalars(&normalized);
    if (core::validate_order(normalized) != core::OrderValidationError::None) {
        return;
    }

    std::lock_guard<std::mutex> lock(reservations_mutex_);
    const auto it = reservations_.find(normalized.order_id);
    if (it == reservations_.end()) {
        return;
    }

    const int64_t release_lots = std::min(normalized.quantity_lots, it->second.remaining_lots);
    if (release_lots <= 0) {
        return;
    }

    int64_t release_units = core::to_notional_units(it->second.reserved_price_ticks, release_lots);
    if (it->second.side == SIDE_SELL) {
        release_units = -release_units;
    }
    committed_exposure_units_.fetch_sub(release_units, std::memory_order_acq_rel);
    it->second.remaining_lots -= release_lots;
    if (it->second.remaining_lots <= 0) {
        reservations_.erase(it);
    }
}

double RiskManager::committed_exposure() const {
    return static_cast<double>(committed_exposure_units_.load(std::memory_order_acquire)) /
           static_cast<double>(core::kNotionalScale);
}

double RiskManager::filled_exposure() const {
    return static_cast<double>(filled_exposure_units_.load(std::memory_order_acquire)) /
           static_cast<double>(core::kNotionalScale);
}

int64_t RiskManager::committed_exposure_units() const {
    return committed_exposure_units_.load(std::memory_order_acquire);
}

int64_t RiskManager::filled_exposure_units() const {
    return filled_exposure_units_.load(std::memory_order_acquire);
}

double RiskManager::net_position_for_symbol(const std::string& symbol) const {
    const std::string key = normalize_symbol(symbol.c_str());
    const auto limit_it = symbol_limit_states_.find(key);
    if (limit_it != symbol_limit_states_.end()) {
        return core::from_quantity_lots(limit_it->second->net_position_lots.load(std::memory_order_acquire));
    }

    std::lock_guard<std::mutex> lock(symbol_positions_mutex_);
    const auto it = positions_.find(key);
    if (it == positions_.end()) {
        return 0.0;
    }
    return core::from_quantity_lots(it->second.net_lots);
}

int64_t RiskManager::signed_notional_units(const Order& order) {
    return core::signed_notional_units(order);
}

int64_t RiskManager::signed_position_lots(const Order& order) {
    return (order.side == SIDE_BUY) ? order.quantity_lots : -order.quantity_lots;
}

std::string RiskManager::normalize_symbol(const char* symbol) {
    std::string out;
    if (!symbol) {
        return out;
    }

    for (const char* p = symbol; *p != '\0'; ++p) {
        if (*p == '/' || *p == '-' || *p == '_' || *p == ' ') {
            continue;
        }
        out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(*p))));
    }
    return out;
}

bool RiskManager::try_add_units(int64_t base, int64_t delta, int64_t* out) {
    if (!out) {
        return false;
    }

    if ((delta > 0 && base > std::numeric_limits<int64_t>::max() - delta) ||
        (delta < 0 && base < std::numeric_limits<int64_t>::min() - delta)) {
        return false;
    }
    *out = base + delta;
    return true;
}

} // namespace argentum::risk
