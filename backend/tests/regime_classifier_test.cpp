#include "regime/regime_classifier.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

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
using argentum::regime::RegimeClassifier;
using argentum::regime::RegimeClassifierConfig;
using argentum::regime::RegimeLabel;

namespace {

constexpr uint64_t kMinute = 60ULL * 1'000'000'000ULL;

struct BarFeeder {
    RegimeClassifier* classifier;
    double price = 100.0;
    uint64_t index = 0;

    // range_frac widens high/low beyond the open/close envelope.
    void feed(double close_multiplier, double range_frac, double volume) {
        const double open = price;
        price = open * close_multiplier;

        Bar bar{};
        bar.start_ts_ns = index * kMinute;
        bar.end_ts_ns = bar.start_ts_ns + kMinute;
        bar.open = open;
        bar.close = price;
        bar.high = std::max(open, price) * (1.0 + range_frac);
        bar.low = std::min(open, price) * (1.0 - range_frac);
        bar.volume = volume;
        bar.tick_count = 10;
        ++index;

        classifier->on_bar(bar);
    }
};

// Alternating 9/11 volumes: mean 10, population stddev 1 — gives the volume
// z-score a well-defined denominator without ever firing expansion (z <= 1).
double baseline_volume(uint64_t i) { return (i % 2 == 0) ? 9.0 : 11.0; }

void test_warmup_is_unknown() {
    RegimeClassifier classifier{RegimeClassifierConfig{}};
    BarFeeder feeder{&classifier};

    for (int i = 0; i < 10; ++i) {
        feeder.feed(1.005, 0.001, baseline_volume(feeder.index));
    }
    CHECK(classifier.current_label() == RegimeLabel::Unknown);
    CHECK_NEAR(classifier.current_confidence(), 0.0, 1e-12);
    CHECK(!classifier.features().ready);
}

void test_steady_uptrend_is_trend() {
    RegimeClassifier classifier{RegimeClassifierConfig{}};
    BarFeeder feeder{&classifier};

    for (int i = 0; i < 80; ++i) {
        feeder.feed(1.005, 0.001, baseline_volume(feeder.index));
    }

    CHECK(classifier.features().ready);
    CHECK(classifier.features().adx >= 25.0);
    CHECK(classifier.current_label() == RegimeLabel::Trend);
    CHECK(classifier.current_confidence() > 0.0);
    CHECK(classifier.current_confidence() < 1.0);  // saturating margin never reaches 1
}

void test_quiet_oscillation_after_volatility_is_range() {
    RegimeClassifier classifier{RegimeClassifierConfig{}};
    BarFeeder feeder{&classifier};

    // Both phases use reciprocal multiplier pairs (f then 1/f) so the series
    // is exactly drift-free: ADX is scale-free and reads even a tiny but
    // CONSISTENT drift as trend, which is correct ADX behavior and not what
    // this test is about.
    // Volatile prefix: +-2% alternating closes with 1% extra range.
    for (int i = 0; i < 60; ++i) {
        feeder.feed((i % 2 == 0) ? 1.02 : 1.0 / 1.02, 0.01, baseline_volume(feeder.index));
    }
    // Quiet phase: +-0.02% oscillation with tiny ranges; ATR decays so the
    // current ATR sits at the bottom of its own recent history.
    for (int i = 0; i < 80; ++i) {
        feeder.feed((i % 2 == 0) ? 1.0002 : 1.0 / 1.0002, 0.0002, baseline_volume(feeder.index));
    }

    CHECK(classifier.features().ready);
    CHECK(classifier.features().adx < 25.0);
    CHECK(classifier.features().atr_percentile <= 0.40);
    CHECK(classifier.current_label() == RegimeLabel::Range);
    CHECK(classifier.current_confidence() > 0.0);
}

void test_stress_beats_trend_priority() {
    RegimeClassifier classifier{RegimeClassifierConfig{}};
    BarFeeder feeder{&classifier};

    // Trending baseline: label must be Trend before the shock.
    for (int i = 0; i < 80; ++i) {
        feeder.feed(1.005, 0.001, baseline_volume(feeder.index));
    }
    CHECK(classifier.current_label() == RegimeLabel::Trend);

    // Violent directional crash with panic volume: the volume z-score and the
    // realized-vol percentile both spike. Even though the move is directional
    // (Trend-compatible), Stress must win by rule priority.
    bool saw_stress = false;
    for (int i = 0; i < 2 && !saw_stress; ++i) {
        feeder.feed(0.97, 0.03, 100.0);
        saw_stress = (classifier.current_label() == RegimeLabel::Stress);
    }
    CHECK(saw_stress);
    CHECK(classifier.current_confidence() > 0.5);  // z-score is enormous vs threshold
}

void test_moderate_volume_spike_is_expansion() {
    RegimeClassifier classifier{RegimeClassifierConfig{}};
    BarFeeder feeder{&classifier};

    // Mild oscillation baseline: ADX low, volumes 9/11 (mean 10, stddev 1).
    for (int i = 0; i < 80; ++i) {
        feeder.feed((i % 2 == 0) ? 1.001 : 0.999, 0.001, baseline_volume(feeder.index));
    }

    // Volume 12 -> z ~= 2.0: above the 1.5 expansion threshold, below the 2.5
    // stress threshold.
    feeder.feed(1.001, 0.001, 12.0);
    CHECK(classifier.current_label() == RegimeLabel::VolExpansion);

    const double z = classifier.features().volume_zscore;
    CHECK(z >= 1.5);
    CHECK(z < 2.5);
}

void test_funding_snapshot_storage() {
    RegimeClassifier classifier{RegimeClassifierConfig{}};
    CHECK(!classifier.features().funding_bps.has_value());
    classifier.on_funding_update(3.5);
    CHECK(classifier.features().funding_bps.has_value());
    CHECK_NEAR(*classifier.features().funding_bps, 3.5, 1e-12);
    classifier.on_funding_update(std::nan(""));  // ignored, keeps last valid value
    CHECK_NEAR(*classifier.features().funding_bps, 3.5, 1e-12);
}

} // namespace

int main() {
    test_warmup_is_unknown();
    test_steady_uptrend_is_trend();
    test_quiet_oscillation_after_volatility_is_range();
    test_stress_beats_trend_priority();
    test_moderate_volume_spike_is_expansion();
    test_funding_snapshot_storage();

    if (g_failures != 0) {
        std::printf("regime_classifier_test FAILED (%d checks)\n", g_failures);
        return 1;
    }
    std::printf("regime_classifier_test passed\n");
    return 0;
}
