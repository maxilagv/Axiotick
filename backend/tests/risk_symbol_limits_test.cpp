#include "risk/risk_manager.hpp"

#include <cassert>
#include <cmath>
#include <cstring>

namespace {

bool almost_equal(double a, double b, double eps = 1e-9) {
    return std::abs(a - b) <= eps;
}

Order make_order(uint64_t id, const char* symbol, Side side, double quantity) {
    Order order{};
    order.order_id = id;
    order.side = static_cast<uint8_t>(side);
    order.type = ORDER_TYPE_LIMIT;
    order.tif = TIF_GTC;
    order.price = 0.000001;
    order.quantity = quantity;
    std::strncpy(order.symbol, symbol, sizeof(order.symbol) - 1);
    return order;
}

}

int main() {
    argentum::risk::RiskManager risk(argentum::risk::RiskLimits{
        1'000'000.0,
        1'000'000.0,
        1'000'000.0,
        {
            {"EUR/USD", 10.0, 10.0}
        }
    });

    Order eur_long_fill = make_order(1001, "EUR/USD", SIDE_BUY, 8.0);
    risk.on_fill(eur_long_fill);
    assert(almost_equal(risk.net_position_for_symbol("EURUSD"), 8.0));

    Order eur_exceed = make_order(1002, "EUR/USD", SIDE_BUY, 3.0);
    assert(!risk.check_order(eur_exceed));

    Order eur_reduce = make_order(1003, "EUR/USD", SIDE_SELL, 3.0);
    assert(risk.check_order(eur_reduce));
    risk.on_cancel(eur_reduce);

    Order gbp_ok = make_order(1004, "GBP/USD", SIDE_BUY, 11.0);
    assert(risk.check_order(gbp_ok));
    risk.on_cancel(gbp_ok);

    return 0;
}
