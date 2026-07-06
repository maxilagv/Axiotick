#include "regime/bar_aggregator.hpp"

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

using argentum::regime::Bar;
using argentum::regime::BarAggregator;

namespace {

constexpr uint64_t kSecond = 1'000'000'000ULL;

MarketTick make_tick(uint64_t ts_ns, double price, double quantity) {
    MarketTick tick{};
    tick.timestamp_ns = ts_ns;
    tick.price = price;
    tick.quantity = quantity;
    std::strncpy(tick.symbol, "BTC/USDT", sizeof(tick.symbol) - 1);
    tick.side = SIDE_BUY;
    return tick;
}

void test_basic_bar_close() {
    BarAggregator agg(60 * kSecond);

    CHECK(!agg.on_tick(make_tick(0 * kSecond + 1, 100.0, 1.0)).has_value());
    CHECK(!agg.on_tick(make_tick(10 * kSecond, 105.0, 2.0)).has_value());
    CHECK(!agg.on_tick(make_tick(59 * kSecond, 95.0, 0.5)).has_value());
    CHECK(agg.has_open_bar());
    CHECK(agg.in_progress_bar().tick_count == 3);

    // The tick that crosses the boundary closes [0, 60) and opens [60, 120).
    const auto closed = agg.on_tick(make_tick(60 * kSecond, 96.0, 1.0));
    CHECK(closed.has_value());
    CHECK(closed->start_ts_ns == 0);
    CHECK(closed->end_ts_ns == 60 * kSecond);
    CHECK_NEAR(closed->open, 100.0, 1e-12);
    CHECK_NEAR(closed->high, 105.0, 1e-12);
    CHECK_NEAR(closed->low, 95.0, 1e-12);
    CHECK_NEAR(closed->close, 95.0, 1e-12);
    CHECK_NEAR(closed->volume, 3.5, 1e-12);
    CHECK(closed->tick_count == 3);

    // The crossing tick belongs to the NEW bar, not the closed one.
    CHECK(agg.in_progress_bar().start_ts_ns == 60 * kSecond);
    CHECK(agg.in_progress_bar().tick_count == 1);
    CHECK_NEAR(agg.in_progress_bar().open, 96.0, 1e-12);
}

void test_gap_produces_single_bar() {
    BarAggregator agg(60 * kSecond);

    CHECK(!agg.on_tick(make_tick(65 * kSecond, 100.0, 1.0)).has_value());  // opens [60,120)

    // A tick 2+ bars later still closes exactly ONE bar (no synthetic empties).
    const auto closed = agg.on_tick(make_tick(185 * kSecond, 101.0, 1.0));
    CHECK(closed.has_value());
    CHECK(closed->start_ts_ns == 60 * kSecond);
    CHECK(closed->end_ts_ns == 120 * kSecond);
    CHECK(agg.in_progress_bar().start_ts_ns == 180 * kSecond);
}

void test_stale_and_invalid_ticks_ignored() {
    BarAggregator agg(60 * kSecond);

    CHECK(!agg.on_tick(make_tick(120 * kSecond, 100.0, 1.0)).has_value());  // opens [120,180)

    // Stale tick from an earlier window: ignored, no emission, bar untouched.
    CHECK(!agg.on_tick(make_tick(30 * kSecond, 500.0, 9.0)).has_value());
    CHECK(agg.in_progress_bar().tick_count == 1);
    CHECK_NEAR(agg.in_progress_bar().high, 100.0, 1e-12);

    // Invalid ticks: ignored.
    CHECK(!agg.on_tick(make_tick(0, 100.0, 1.0)).has_value());
    CHECK(!agg.on_tick(make_tick(130 * kSecond, -5.0, 1.0)).has_value());
    CHECK(agg.in_progress_bar().tick_count == 1);

    // Out-of-order WITHIN the open bar still updates high/low/volume.
    CHECK(!agg.on_tick(make_tick(125 * kSecond, 110.0, 2.0)).has_value());
    CHECK(agg.in_progress_bar().tick_count == 2);
    CHECK_NEAR(agg.in_progress_bar().high, 110.0, 1e-12);
    CHECK_NEAR(agg.in_progress_bar().close, 110.0, 1e-12);
}

void test_zero_quantity_counts_tick_not_volume() {
    BarAggregator agg(60 * kSecond);
    CHECK(!agg.on_tick(make_tick(1, 100.0, 0.0)).has_value());
    CHECK(agg.in_progress_bar().tick_count == 1);
    CHECK_NEAR(agg.in_progress_bar().volume, 0.0, 1e-12);
}

} // namespace

int main() {
    test_basic_bar_close();
    test_gap_produces_single_bar();
    test_stale_and_invalid_ticks_ignored();
    test_zero_quantity_counts_tick_not_volume();

    if (g_failures != 0) {
        std::printf("bar_aggregator_test FAILED (%d checks)\n", g_failures);
        return 1;
    }
    std::printf("bar_aggregator_test passed\n");
    return 0;
}
