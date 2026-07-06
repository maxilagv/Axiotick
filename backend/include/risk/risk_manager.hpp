#pragma once

#include "core/fixed_point.hpp"
#include "core/types.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace argentum::risk {

struct SymbolRiskLimit {
    std::string symbol;
    double max_net_position = 0.0;
    double max_gross_position = 0.0;
};

struct RiskLimits {
    double max_order_value = 0.0;
    double max_position_exposure = 0.0;
    double max_daily_loss = 0.0;   // > 0 enables the automatic kill switch on daily PnL breach
    std::vector<SymbolRiskLimit> symbol_limits{};
};

class RiskManager {
public:
    explicit RiskManager(RiskLimits limits);

    bool check_order(const Order& order);
    void on_fill(const Order& order);
    void on_cancel(const Order& order);

    /// Updates the mark price for a symbol and refreshes unrealized PnL.
    /// May trigger the kill switch if the daily loss limit is breached.
    void mark_to_market(const std::string& symbol, double price);

    /// Rolls the daily PnL baseline when now_ns crosses a UTC day boundary.
    /// The kill switch is intentionally NOT reset by a day roll (manual only).
    void maybe_roll_day(uint64_t now_ns);

    /// Realized + unrealized PnL since the last day roll (or process start).
    double daily_pnl() const;
    /// Cumulative realized PnL since process start.
    double realized_pnl() const;
    /// Current unrealized PnL across all symbols at last mark prices.
    double unrealized_pnl() const;

    bool kill_switch_active() const;
    void trigger_kill_switch(const std::string& reason);
    void reset_kill_switch(const std::string& reason);

    double committed_exposure() const;
    double filled_exposure() const;
    int64_t committed_exposure_units() const;
    int64_t filled_exposure_units() const;
    double net_position_for_symbol(const std::string& symbol) const;

private:
    struct Reservation {
        Side side = SIDE_BUY;
        int64_t reserved_price_ticks = 0;
        int64_t remaining_lots = 0;
        std::string symbol;
    };

    struct SymbolLimitState {
        SymbolRiskLimit limit;
        std::atomic<int64_t> net_position_lots{0};
    };

    // Average-cost position accounting per symbol; guarded by symbol_positions_mutex_.
    struct PositionState {
        int64_t net_lots = 0;
        double avg_price = 0.0;
        double last_mark_price = 0.0;
        double unrealized_pnl = 0.0;
    };

    static int64_t signed_notional_units(const Order& order);
    static int64_t signed_position_lots(const Order& order);
    static std::string normalize_symbol(const char* symbol);
    static bool try_add_units(int64_t base, int64_t delta, int64_t* out);

    // Callers must hold symbol_positions_mutex_.
    void update_position_on_fill_locked(const std::string& symbol_key, uint8_t side, double price, int64_t lots);
    void refresh_unrealized_locked(PositionState& position);
    void check_daily_loss_locked();
    double daily_pnl_locked() const;

    RiskLimits limits_;
    std::atomic<int64_t> committed_exposure_units_{0};
    std::atomic<int64_t> filled_exposure_units_{0};
    std::atomic<bool> kill_switch_active_{false};
    mutable std::mutex reservations_mutex_;
    mutable std::mutex symbol_positions_mutex_;
    std::unordered_map<uint64_t, Reservation> reservations_;
    std::unordered_map<std::string, std::unique_ptr<SymbolLimitState>> symbol_limit_states_;
    std::unordered_map<std::string, PositionState> positions_;
    double realized_pnl_total_ = 0.0;
    double unrealized_pnl_total_ = 0.0;
    double day_baseline_pnl_ = 0.0;
    uint64_t current_day_index_ = 0;
};

} // namespace argentum::risk
