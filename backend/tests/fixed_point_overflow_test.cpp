#include "core/fixed_point.hpp"

#include <cassert>
#include <cstdint>
#include <limits>

int main() {
    using namespace argentum::core;

    assert(to_price_ticks(kMaxSafePrice) != std::numeric_limits<int64_t>::max());
    assert(to_quantity_lots(kMaxSafeQuantity) != std::numeric_limits<int64_t>::max());
    assert(to_price_ticks(kMaxSafePrice + 1.0) == std::numeric_limits<int64_t>::max());
    assert(to_quantity_lots(kMaxSafeQuantity + 1.0) == std::numeric_limits<int64_t>::max());

    Order order{};
    order.order_id = 1;
    order.side = SIDE_BUY;
    order.type = ORDER_TYPE_LIMIT;
    order.tif = TIF_GTC;
    order.price = kMaxSafePrice + 1.0;
    order.quantity = 1.0;
    normalize_order_scalars(&order);
    assert(order.price_ticks == std::numeric_limits<int64_t>::max());
    assert(order.quantity_lots == std::numeric_limits<int64_t>::max());

    assert(to_notional_units(kMaxPriceTicks, kMaxQuantityLots) != std::numeric_limits<int64_t>::max());
    assert(to_notional_units(kMaxPriceTicks + 1, 1) == std::numeric_limits<int64_t>::max());
    assert(to_notional_units(1, kMaxQuantityLots + 1) == std::numeric_limits<int64_t>::max());

    return 0;
}
