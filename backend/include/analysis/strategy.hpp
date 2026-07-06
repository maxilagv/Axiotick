#pragma once

#include "core/types.h"
#include <vector>
#include <string>
#include <numeric>

namespace argentum::analysis {

/**
 * @brief Base interface for trading strategies.
 */
class Strategy {
public:
    virtual ~Strategy() = default;
    
    virtual void on_tick(const MarketTick& tick) = 0;
    virtual void on_order_update(const Order& order) = 0;
    
    [[nodiscard]] virtual std::string get_name() const = 0;
};

/**
 * @brief Simple Moving Average (SMA) calculator.
 * Templated for precision flexibility (float/double).
 */
template <typename T>
class SMA {
public:
    explicit SMA(size_t period) : period_(period) {}

    void add(T value) {
        values_.push_back(value);
        sum_ += value;
        if (values_.size() > period_) {
            sum_ -= values_.front();
            values_.erase(values_.begin());
        }
    }

    [[nodiscard]] T value() const {
        if (values_.empty()) return 0;
        return sum_ / values_.size();
    }

    [[nodiscard]] bool is_ready() const {
        return values_.size() == period_;
    }

private:
    size_t period_;
    std::vector<T> values_;
    T sum_ = 0;
};

} // namespace argentum::analysis
