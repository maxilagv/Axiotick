#pragma once

#include "core/types.h"

#include <cstdint>
#include <limits>

namespace argentum::core {

enum class OrderValidationError : uint8_t {
    None = 0,
    ZeroId = 1,
    ZeroQuantity = 2,
    InvalidSide = 3,
    InvalidType = 4,
    InvalidTimeInForce = 5,
    InvalidPrice = 6,
    OverflowScalar = 7
};

inline OrderValidationError validate_order(const Order& order) {
    if (order.order_id == 0) {
        return OrderValidationError::ZeroId;
    }
    if (order.quantity_lots <= 0) {
        return OrderValidationError::ZeroQuantity;
    }
    if (order.side != SIDE_BUY && order.side != SIDE_SELL) {
        return OrderValidationError::InvalidSide;
    }
    if (order.type != ORDER_TYPE_MARKET &&
        order.type != ORDER_TYPE_LIMIT &&
        order.type != ORDER_TYPE_STOP) {
        return OrderValidationError::InvalidType;
    }
    if (order.tif != TIF_GTC &&
        order.tif != TIF_IOC &&
        order.tif != TIF_FOK) {
        return OrderValidationError::InvalidTimeInForce;
    }
    if (order.price_ticks <= 0) {
        return OrderValidationError::InvalidPrice;
    }
    if (order.price_ticks == std::numeric_limits<int64_t>::max() ||
        order.quantity_lots == std::numeric_limits<int64_t>::max()) {
        return OrderValidationError::OverflowScalar;
    }
    return OrderValidationError::None;
}

inline bool is_valid_order(const Order& order) {
    return validate_order(order) == OrderValidationError::None;
}

} // namespace argentum::core
