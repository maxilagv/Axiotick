#pragma once

#include "core/types.h"

#include <cmath>
#include <cstdint>
#include <limits>

namespace argentum::core {

constexpr int64_t kPriceScale = 1'000'000;      // 1 tick = 1e-6
constexpr int64_t kQuantityScale = 1'000'000;   // 1 lot = 1e-6
constexpr int64_t kNotionalScale = kPriceScale * kQuantityScale;
constexpr double kMaxSafePrice = 999999.0;
constexpr double kMaxSafeQuantity = 1000.0;
constexpr int64_t kMaxPriceTicks = static_cast<int64_t>(kMaxSafePrice * static_cast<double>(kPriceScale));
constexpr int64_t kMaxQuantityLots = static_cast<int64_t>(kMaxSafeQuantity * static_cast<double>(kQuantityScale));
constexpr long double kMaxSupportedPriceQuantityProduct =
    static_cast<long double>(std::numeric_limits<int64_t>::max()) /
    static_cast<long double>(kNotionalScale);

static_assert(
    kMaxSupportedPriceQuantityProduct > 0.0L,
    "Fixed-point range must support a positive real-valued price*quantity product.");

inline int64_t round_to_i64(double value, int64_t scale) {
    if (!std::isfinite(value)) return 0;
    const long double scaled = static_cast<long double>(value) * static_cast<long double>(scale);
    if (scaled > static_cast<long double>(std::numeric_limits<int64_t>::max())) {
        return std::numeric_limits<int64_t>::max();
    }
    if (scaled < static_cast<long double>(std::numeric_limits<int64_t>::min())) {
        return std::numeric_limits<int64_t>::min();
    }
    return static_cast<int64_t>(std::llround(scaled));
}

inline double to_double(int64_t value, int64_t scale) {
    return static_cast<double>(value) / static_cast<double>(scale);
}

inline int64_t to_price_ticks(double price) {
    if (!std::isfinite(price) || std::fabs(price) > kMaxSafePrice) {
        return std::numeric_limits<int64_t>::max();
    }
    return round_to_i64(price, kPriceScale);
}

inline int64_t to_quantity_lots(double quantity) {
    if (!std::isfinite(quantity) || std::fabs(quantity) > kMaxSafeQuantity) {
        return std::numeric_limits<int64_t>::max();
    }
    return round_to_i64(quantity, kQuantityScale);
}

inline double from_price_ticks(int64_t ticks) {
    return to_double(ticks, kPriceScale);
}

inline double from_quantity_lots(int64_t lots) {
    return to_double(lots, kQuantityScale);
}

inline int64_t to_notional_units(int64_t price_ticks, int64_t quantity_lots) {
    if (std::llabs(price_ticks) > kMaxPriceTicks || std::llabs(quantity_lots) > kMaxQuantityLots) {
        return std::numeric_limits<int64_t>::max();
    }
    const long double product = static_cast<long double>(price_ticks) * static_cast<long double>(quantity_lots);
    if (product > static_cast<long double>(std::numeric_limits<int64_t>::max())) {
        return std::numeric_limits<int64_t>::max();
    }
    if (product < static_cast<long double>(std::numeric_limits<int64_t>::min())) {
        return std::numeric_limits<int64_t>::min();
    }
    return static_cast<int64_t>(product);
}

inline void normalize_order_scalars(Order* order) {
    if (!order) return;

    if (order->price_ticks == 0 && order->price != 0.0) {
        order->price_ticks = to_price_ticks(order->price);
    }
    if (order->quantity_lots == 0 && order->quantity != 0.0) {
        order->quantity_lots = to_quantity_lots(order->quantity);
    }

    if (order->price == 0.0 && order->price_ticks != 0) {
        order->price = from_price_ticks(order->price_ticks);
    }
    if (order->quantity == 0.0 && order->quantity_lots != 0) {
        order->quantity = from_quantity_lots(order->quantity_lots);
    }

    if (std::llabs(order->price_ticks) > kMaxPriceTicks ||
        std::llabs(order->quantity_lots) > kMaxQuantityLots) {
        order->price_ticks = std::numeric_limits<int64_t>::max();
        order->quantity_lots = std::numeric_limits<int64_t>::max();
    }

    if (order->tif == 0) {
        order->tif = TIF_GTC;
    }
}

inline int64_t signed_notional_units(const Order& order) {
    const int64_t raw = to_notional_units(order.price_ticks, order.quantity_lots);
    return (order.side == SIDE_BUY) ? raw : -raw;
}

} // namespace argentum::core
