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

void test_manual_trigger_blocks_new_orders() {
    RiskManager risk(RiskLimits{1e9, 1e9, 0.0});  // 0 disables the automatic breach path

    Order order = make_order(1, SIDE_BUY, 100.0, 1.0);
    CHECK(risk.check_order(order));
    CHECK(!risk.kill_switch_active());

    risk.trigger_kill_switch("operator_manual");
    CHECK(risk.kill_switch_active());
    CHECK(!risk.check_order(make_order(2, SIDE_BUY, 100.0, 1.0)));

    // Re-triggering is idempotent.
    risk.trigger_kill_switch("operator_manual_again");
    CHECK(risk.kill_switch_active());

    // In-flight fills must still be accounted while the switch is active:
    // blocking accounting would corrupt position/PnL truth.
    risk.on_fill(order);
    CHECK_NEAR(risk.net_position_for_symbol("BTC/USDT"), 1.0, 1e-9);

    risk.reset_kill_switch("operator_reset");
    CHECK(!risk.kill_switch_active());
    CHECK(risk.check_order(make_order(3, SIDE_BUY, 100.0, 1.0)));

    // Resetting an inactive switch is a no-op.
    risk.reset_kill_switch("noop");
    CHECK(!risk.kill_switch_active());
}

void test_day_roll_does_not_reset_kill_switch() {
    RiskManager risk(RiskLimits{1e9, 1e9, 50.0});

    const uint64_t day0 = 19'900ULL * kNsPerDay;
    risk.maybe_roll_day(day0);

    risk.on_fill(make_order(10, SIDE_BUY, 100.0, 1.0));
    risk.on_fill(make_order(11, SIDE_SELL, 30.0, 1.0));
    CHECK_NEAR(risk.daily_pnl(), -70.0, 1e-9);
    CHECK(risk.kill_switch_active());

    risk.maybe_roll_day(day0 + kNsPerDay);
    CHECK_NEAR(risk.daily_pnl(), 0.0, 1e-9);
    // The new day does NOT re-enable trading: reset is a manual decision.
    CHECK(risk.kill_switch_active());
    CHECK(!risk.check_order(make_order(12, SIDE_BUY, 100.0, 1.0)));

    risk.reset_kill_switch("operator_after_review");
    CHECK(risk.check_order(make_order(13, SIDE_BUY, 100.0, 1.0)));
}

void test_zero_limit_disables_auto_trigger() {
    RiskManager risk(RiskLimits{1e9, 1e9, 0.0});

    risk.on_fill(make_order(20, SIDE_BUY, 100.0, 1.0));
    risk.on_fill(make_order(21, SIDE_SELL, 1.0, 1.0));  // realized -99
    CHECK_NEAR(risk.daily_pnl(), -99.0, 1e-9);
    CHECK(!risk.kill_switch_active());
    CHECK(risk.check_order(make_order(22, SIDE_BUY, 100.0, 1.0)));
}

} // namespace

int main() {
    test_manual_trigger_blocks_new_orders();
    test_day_roll_does_not_reset_kill_switch();
    test_zero_limit_disables_auto_trigger();

    if (g_failures != 0) {
        std::printf("risk_kill_switch_test FAILED (%d checks)\n", g_failures);
        return 1;
    }
    std::printf("risk_kill_switch_test passed\n");
    return 0;
}
