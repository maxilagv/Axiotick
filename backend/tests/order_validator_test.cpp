#include "core/fixed_point.hpp"
#include "core/order_validator.hpp"

#include <cassert>
#include <limits>

int main() {
    Order valid{};
    valid.order_id = 1;
    valid.side = SIDE_BUY;
    valid.type = ORDER_TYPE_LIMIT;
    valid.tif = TIF_GTC;
    valid.price = 100.0;
    valid.quantity = 1.0;
    argentum::core::normalize_order_scalars(&valid);
    assert(argentum::core::validate_order(valid) == argentum::core::OrderValidationError::None);

    Order zero_id = valid;
    zero_id.order_id = 0;
    assert(argentum::core::validate_order(zero_id) == argentum::core::OrderValidationError::ZeroId);

    Order invalid_side = valid;
    invalid_side.side = 9;
    assert(argentum::core::validate_order(invalid_side) == argentum::core::OrderValidationError::InvalidSide);

    Order overflow = valid;
    overflow.price_ticks = std::numeric_limits<int64_t>::max();
    assert(argentum::core::validate_order(overflow) == argentum::core::OrderValidationError::OverflowScalar);

    return 0;
}
