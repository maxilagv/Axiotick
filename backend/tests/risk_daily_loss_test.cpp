#include "risk/risk_manager.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>

static int g_failures = 0;
#define CHECK(cond) \
    do { \
        if (!(cond)) { \
            std::printf("CHECK FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures; \
        } \
    } while (0)
#define CHECK_NEAR(a, b, eps) CHECK(std::abs((a) - (b)) <= (eps))

using argentum::risk::RiskLimits;
using argentum::risk::RiskManager;

namespace {

constexpr uint64_t kNsPerDay = 86'400ULL * 1'000'000'000ULL;

Order make_order(uint64_t id, uint8_t side, double price, double quantity) {
    Order order{};
    order.order_id = id;
    order.side = side;
    order.type = ORDER_TYPE_LIMIT;
    order.price = price;
    order.quantity = quantity;
    std::strncpy(order.symbol, "BTC/USDT", sizeof(order.symbol) - 1);
    return order;
}

void test_realized_loss_triggers_kill_switch() {
    RiskManager risk(RiskLimits{1e9, 1e9, 50.0});

    Order buy = make_order(1, SIDE_BUY, 100.0, 1.0);
    CHECK(risk.check_order(buy));
    risk.on_fill(buy);
    CHECK_NEAR(risk.realized_pnl(), 0.0, 1e-9);
    CHECK(!risk.kill_switch_active());

    // Close the long at 40: realized -60 breaches the 50 daily loss limit.
    Order sell = make_order(2, SIDE_SELL, 40.0, 1.0);
    risk.on_fill(sell);
    CHECK_NEAR(risk.realized_pnl(), -60.0, 1e-9);
    CHECK_NEAR(risk.daily_pnl(), -60.0, 1e-9);
    CHECK(risk.kill_switch_active());

    // While active, no new order passes.
    Order next = make_order(3, SIDE_BUY, 100.0, 0.1);
    CHECK(!risk.check_order(next));

    // Manual reset restores order flow (daily PnL is unchanged by the reset).
    risk.reset_kill_switch("test_reset");
    CHECK(!risk.kill_switch_active());
    CHECK(risk.check_order(next));
}

void test_unrealized_breach_via_mark_to_market() {
    RiskManager risk(RiskLimits{1e9, 1e9, 50.0});

    Order buy = make_order(10, SIDE_BUY, 100.0, 1.0);
    CHECK(risk.check_order(buy));
    risk.on_fill(buy);

    risk.mark_to_market("BTC/USDT", 60.0);
    CHECK_NEAR(risk.unrealized_pnl(), -40.0, 1e-9);
    CHECK(!risk.kill_switch_active());

    risk.mark_to_market("BTC/USDT", 45.0);
    CHECK_NEAR(risk.unrealized_pnl(), -55.0, 1e-9);
    CHECK_NEAR(risk.daily_pnl(), -55.0, 1e-9);
    CHECK(risk.kill_switch_active());

    // Marks for unknown symbols or invalid prices are ignored.
    risk.mark_to_market("UNKNOWN/PAIR", 100.0);
    risk.mark_to_market("BTC/USDT", -5.0);
    CHECK_NEAR(risk.unrealized_pnl(), -55.0, 1e-9);
}

void test_average_cost_accounting_and_flip() {
    RiskManager risk(RiskLimits{1e9, 1e9, 1e9});

    risk.on_fill(make_order(20, SIDE_BUY, 100.0, 1.0));
    risk.on_fill(make_order(21, SIDE_BUY, 110.0, 1.0));
    CHECK_NEAR(risk.net_position_for_symbol("BTC/USDT"), 2.0, 1e-9);
    CHECK_NEAR(risk.realized_pnl(), 0.0, 1e-9);

    // Sell 1 @ 120 against avg 105 -> +15 realized.
    risk.on_fill(make_order(22, SIDE_SELL, 120.0, 1.0));
    CHECK_NEAR(risk.realized_pnl(), 15.0, 1e-9);
    CHECK_NEAR(risk.net_position_for_symbol("BTC/USDT"), 1.0, 1e-9);

    // Sell 2 @ 100: closes remaining 1 (-5) and flips short 1 @ 100.
    risk.on_fill(make_order(23, SIDE_SELL, 100.0, 2.0));
    CHECK_NEAR(risk.realized_pnl(), 10.0, 1e-9);
    CHECK_NEAR(risk.net_position_for_symbol("BTC/USDT"), -1.0, 1e-9);

    // Mark the short at 90 -> +10 unrealized.
    risk.mark_to_market("BTC/USDT", 90.0);
    CHECK_NEAR(risk.unrealized_pnl(), 10.0, 1e-9);
    CHECK_NEAR(risk.daily_pnl(), 20.0, 1e-9);
    CHECK(!risk.kill_switch_active());
}

void test_day_roll_rebases_daily_pnl() {
    RiskManager risk(RiskLimits{1e9, 1e9, 100.0});

    const uint64_t day0 = 19'900ULL * kNsPerDay;
    risk.maybe_roll_day(day0);

    risk.on_fill(make_order(30, SIDE_BUY, 100.0, 1.0));
    risk.on_fill(make_order(31, SIDE_SELL, 70.0, 1.0));
    CHECK_NEAR(risk.daily_pnl(), -30.0, 1e-9);
    CHECK(!risk.kill_switch_active());

    // Next UTC day: baseline moves, daily PnL resets, cumulative stays.
    risk.maybe_roll_day(day0 + kNsPerDay);
    CHECK_NEAR(risk.daily_pnl(), 0.0, 1e-9);
    CHECK_NEAR(risk.realized_pnl(), -30.0, 1e-9);

    // A same-size loss today only counts against today's budget.
    risk.on_fill(make_order(32, SIDE_BUY, 200.0, 1.0));
    risk.on_fill(make_order(33, SIDE_SELL, 130.0, 1.0));
    CHECK_NEAR(risk.daily_pnl(), -70.0, 1e-9);
    CHECK(!risk.kill_switch_active());

    risk.on_fill(make_order(34, SIDE_BUY, 200.0, 1.0));
    risk.on_fill(make_order(35, SIDE_SELL, 160.0, 1.0));
    CHECK_NEAR(risk.daily_pnl(), -110.0, 1e-9);
    CHECK(risk.kill_switch_active());
}

} // namespace

int main() {
    test_realized_loss_triggers_kill_switch();
    test_unrealized_breach_via_mark_to_market();
    test_average_cost_accounting_and_flip();
    test_day_roll_rebases_daily_pnl();

    if (g_failures != 0) {
        std::printf("risk_daily_loss_test FAILED (%d checks)\n", g_failures);
        return 1;
    }
    std::printf("risk_daily_loss_test passed\n");
    return 0;
}
