#pragma once

#include <cmath>
#include <cstddef>
#include <deque>

namespace argentum::regime {

/**
 * @brief Fixed-window mean/stddev over a stream of values. O(1) per push via
 * running sum/sum-of-squares (adequate precision for feature windows of
 * 20-100 values; not intended for million-element windows).
 */
class RollingWindowStat {
public:
    explicit RollingWindowStat(size_t window) : window_(window == 0 ? 1 : window) {}

    void push(double value) {
        values_.push_back(value);
        sum_ += value;
        sum_sq_ += value * value;
        if (values_.size() > window_) {
            const double dropped = values_.front();
            values_.pop_front();
            sum_ -= dropped;
            sum_sq_ -= dropped * dropped;
        }
    }

    [[nodiscard]] bool ready() const { return values_.size() >= window_; }
    [[nodiscard]] size_t count() const { return values_.size(); }

    [[nodiscard]] double mean() const {
        if (values_.empty()) return 0.0;
        return sum_ / static_cast<double>(values_.size());
    }

    [[nodiscard]] double stddev() const {
        if (values_.size() < 2) return 0.0;
        const double n = static_cast<double>(values_.size());
        const double m = sum_ / n;
        const double variance = sum_sq_ / n - m * m;
        return (variance > 0.0) ? std::sqrt(variance) : 0.0;
    }

private:
    size_t window_;
    std::deque<double> values_;
    double sum_ = 0.0;
    double sum_sq_ = 0.0;
};

/**
 * @brief Empirical percentile of a value against a rolling window of recent
 * observations, using the midrank convention for ties:
 * (count_less + 0.5 * count_equal) / n. A constant series therefore reads as
 * percentile 0.5 (neither extreme), which keeps "low vs. own history" rules
 * from firing spuriously on flat inputs.
 */
class PercentileWindow {
public:
    explicit PercentileWindow(size_t window) : window_(window == 0 ? 1 : window) {}

    void push(double value) {
        values_.push_back(value);
        if (values_.size() > window_) {
            values_.pop_front();
        }
    }

    [[nodiscard]] size_t count() const { return values_.size(); }
    [[nodiscard]] bool ready(size_t min_samples) const { return values_.size() >= min_samples; }

    [[nodiscard]] double midrank_percentile(double value) const {
        if (values_.empty()) return 0.5;
        size_t less = 0;
        size_t equal = 0;
        for (double v : values_) {
            if (v < value) ++less;
            else if (v == value) ++equal;
        }
        return (static_cast<double>(less) + 0.5 * static_cast<double>(equal)) /
               static_cast<double>(values_.size());
    }

private:
    size_t window_;
    std::deque<double> values_;
};

/**
 * @brief Wilder's directional movement system: ADX(period) and ATR(period).
 *
 * Standard construction: +DM/-DM/TR accumulated over the first `period` bars,
 * then Wilder-smoothed (S = S - S/period + X). DX is averaged over `period`
 * values for the first ADX, then Wilder-smoothed. ADX becomes ready after
 * 1 + 2*period bars (29 for the default 14); ATR after 1 + period bars.
 */
class WilderDmi {
public:
    explicit WilderDmi(size_t period = 14) : period_(period == 0 ? 1 : period) {}

    void on_bar(double high, double low, double close) {
        if (!has_prev_) {
            set_prev(high, low, close);
            has_prev_ = true;
            return;
        }

        const double up_move = high - prev_high_;
        const double down_move = prev_low_ - low;
        const double plus_dm = (up_move > down_move && up_move > 0.0) ? up_move : 0.0;
        const double minus_dm = (down_move > up_move && down_move > 0.0) ? down_move : 0.0;
        const double tr = std::max(high - low,
                          std::max(std::abs(high - prev_close_), std::abs(low - prev_close_)));

        const double period = static_cast<double>(period_);
        if (dm_count_ < period_) {
            sm_plus_dm_ += plus_dm;
            sm_minus_dm_ += minus_dm;
            sm_tr_ += tr;
            ++dm_count_;
            if (dm_count_ < period_) {
                set_prev(high, low, close);
                return;
            }
        } else {
            sm_plus_dm_ += plus_dm - sm_plus_dm_ / period;
            sm_minus_dm_ += minus_dm - sm_minus_dm_ / period;
            sm_tr_ += tr - sm_tr_ / period;
        }

        double plus_di = 0.0;
        double minus_di = 0.0;
        if (sm_tr_ > 1e-12) {
            plus_di = 100.0 * sm_plus_dm_ / sm_tr_;
            minus_di = 100.0 * sm_minus_dm_ / sm_tr_;
        }
        const double di_sum = plus_di + minus_di;
        const double dx = (di_sum > 1e-12) ? 100.0 * std::abs(plus_di - minus_di) / di_sum : 0.0;

        if (!adx_ready_) {
            dx_sum_ += dx;
            ++dx_count_;
            if (dx_count_ >= period_) {
                adx_ = dx_sum_ / period;
                adx_ready_ = true;
            }
        } else {
            adx_ = (adx_ * (period - 1.0) + dx) / period;
        }

        set_prev(high, low, close);
    }

    [[nodiscard]] bool atr_ready() const { return dm_count_ >= period_; }
    [[nodiscard]] bool ready() const { return adx_ready_; }
    [[nodiscard]] double adx() const { return adx_; }
    [[nodiscard]] double atr() const {
        return atr_ready() ? sm_tr_ / static_cast<double>(period_) : 0.0;
    }

private:
    void set_prev(double high, double low, double close) {
        prev_high_ = high;
        prev_low_ = low;
        prev_close_ = close;
    }

    size_t period_;
    bool has_prev_ = false;
    double prev_high_ = 0.0;
    double prev_low_ = 0.0;
    double prev_close_ = 0.0;
    size_t dm_count_ = 0;
    double sm_plus_dm_ = 0.0;
    double sm_minus_dm_ = 0.0;
    double sm_tr_ = 0.0;
    size_t dx_count_ = 0;
    double dx_sum_ = 0.0;
    double adx_ = 0.0;
    bool adx_ready_ = false;
};

} // namespace argentum::regime
