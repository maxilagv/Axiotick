#include "risk/risk_manager.hpp"

#include <cassert>
#include <cmath>
#include <cstring>

namespace {
bool almost_equal(double a, double b, double eps = 1e-9) {
    return std::abs(a - b) <= eps;
}
}

int main() {
    argentum::risk::RiskManager risk(argentum::risk::RiskLimits{
        10'000'000.0,
        10'000'000.0,
        10'000'000.0
    });

    Order buy{};
    buy.order_id = 1001;
    buy.side = SIDE_BUY;
    buy.type = ORDER_TYPE_LIMIT;
    buy.price = 100.0;
    buy.quantity = 2.0;
    std::strncpy(buy.symbol, "BTC/USDT", sizeof(buy.symbol) - 1);

    assert(risk.check_order(buy));
    assert(almost_equal(risk.committed_exposure(), 200.0));

    Order buy_fill = buy;
    buy_fill.price = 120.0;
    buy_fill.quantity = 1.0;
    buy_fill.price_ticks = 0;
    buy_fill.quantity_lots = 0;
    risk.on_fill(buy_fill);

    // Reserved notional is released at reserved price, fill uses executed price.
    assert(almost_equal(risk.committed_exposure(), 100.0));
    assert(almost_equal(risk.filled_exposure(), 120.0));

    Order buy_cancel = buy;
    buy_cancel.quantity = 1.0;
    buy_cancel.price_ticks = 0;
    buy_cancel.quantity_lots = 0;
    risk.on_cancel(buy_cancel);
    assert(almost_equal(risk.committed_exposure(), 0.0));

    Order sell{};
    sell.order_id = 2002;
    sell.side = SIDE_SELL;
    sell.type = ORDER_TYPE_LIMIT;
    sell.price = 50.0;
    sell.quantity = 3.0;
    std::strncpy(sell.symbol, "BTC/USDT", sizeof(sell.symbol) - 1);

    assert(risk.check_order(sell));
    assert(almost_equal(risk.committed_exposure(), -150.0));

    Order sell_cancel = sell;
    sell_cancel.quantity = 1.0;
    sell_cancel.price_ticks = 0;
    sell_cancel.quantity_lots = 0;
    risk.on_cancel(sell_cancel);
    assert(almost_equal(risk.committed_exposure(), -100.0));

    Order sell_fill = sell;
    sell_fill.price = 40.0;
    sell_fill.quantity = 2.0;
    sell_fill.price_ticks = 0;
    sell_fill.quantity_lots = 0;
    risk.on_fill(sell_fill);
    assert(almost_equal(risk.committed_exposure(), 0.0));

    // 120 from buy fill and -80 from sell fill.
    assert(almost_equal(risk.filled_exposure(), 40.0));

    return 0;
}
