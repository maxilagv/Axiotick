#pragma once

#include "core/types.h"

#include <cstdint>
#include <optional>

namespace argentum::regime {

/**
 * @brief One closed OHLCV bar. start_ts_ns is aligned to the bar duration;
 * end_ts_ns is exclusive (start + duration).
 */
struct Bar {
    uint64_t start_ts_ns = 0;
    uint64_t end_ts_ns = 0;
    double open = 0.0;
    double high = 0.0;
    double low = 0.0;
    double close = 0.0;
    double volume = 0.0;
    uint32_t tick_count = 0;
};

/**
 * @brief Incremental tick -> time-bar aggregator. O(1) per tick, no growing
 * containers, no bus dependency — pure computation usable both by the live
 * pipeline and by backtest loops (same class, same numbers).
 *
 * Semantics (fixed on purpose, tested in bar_aggregator_test):
 * - Bars are aligned to wall-clock boundaries (timestamp_ns / duration).
 * - The tick that crosses a boundary CLOSES the previous bar and OPENS the
 *   next one; it is not part of the bar being closed.
 * - No synthetic empty bars are emitted across liquidity gaps: a gap of N
 *   bar-durations still produces exactly one closed bar (the last one that
 *   had ticks). Known v1 simplification; revisit with live-feed evidence.
 * - Ticks older than the currently open bar (stale/out-of-order across a
 *   boundary) are ignored rather than corrupting a closed bar. Out-of-order
 *   ticks WITHIN the open bar still update high/low/volume; open/close follow
 *   arrival order.
 * - Invalid ticks (zero timestamp or non-positive price) are ignored.
 */
class BarAggregator {
public:
    explicit BarAggregator(uint64_t bar_duration_ns)
        : bar_duration_ns_(bar_duration_ns == 0 ? 1 : bar_duration_ns) {}

    std::optional<Bar> on_tick(const MarketTick& tick) {
        if (tick.timestamp_ns == 0 || !(tick.price > 0.0)) {
            return std::nullopt;
        }

        const uint64_t bar_start = tick.timestamp_ns - (tick.timestamp_ns % bar_duration_ns_);

        if (!has_open_bar()) {
            open_bar(bar_start, tick);
            return std::nullopt;
        }

        if (bar_start < current_.start_ts_ns) {
            return std::nullopt;  // stale tick from an already-closed window
        }

        if (bar_start == current_.start_ts_ns) {
            apply_tick(tick);
            return std::nullopt;
        }

        Bar closed = current_;
        open_bar(bar_start, tick);
        return closed;
    }

    [[nodiscard]] bool has_open_bar() const { return current_.tick_count > 0; }
    [[nodiscard]] const Bar& in_progress_bar() const { return current_; }
    [[nodiscard]] uint64_t bar_duration_ns() const { return bar_duration_ns_; }

private:
    void open_bar(uint64_t bar_start, const MarketTick& tick) {
        current_ = Bar{};
        current_.start_ts_ns = bar_start;
        current_.end_ts_ns = bar_start + bar_duration_ns_;
        current_.open = tick.price;
        current_.high = tick.price;
        current_.low = tick.price;
        current_.close = tick.price;
        current_.volume = (tick.quantity > 0.0) ? tick.quantity : 0.0;
        current_.tick_count = 1;
    }

    void apply_tick(const MarketTick& tick) {
        if (tick.price > current_.high) current_.high = tick.price;
        if (tick.price < current_.low) current_.low = tick.price;
        current_.close = tick.price;
        if (tick.quantity > 0.0) current_.volume += tick.quantity;
        ++current_.tick_count;
    }

    uint64_t bar_duration_ns_;
    Bar current_{};
};

} // namespace argentum::regime
