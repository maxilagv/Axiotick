#pragma once

#include "regime/bar_aggregator.hpp"
#include "regime/regime_features.hpp"
#include "regime/regime_types.hpp"

#include <cstdint>
#include <optional>

namespace argentum::regime {

/**
 * @brief Thresholds for the deterministic rule-based classifier.
 *
 * These defaults are hand-set engineering conventions (ADX 25 is the standard
 * trend threshold). Block 6 replaces them with HMM/GMM-calibrated values
 * loaded from a versioned JSON — the rule structure stays identical, only the
 * numbers become statistically derived.
 */
struct RegimeClassifierConfig {
    uint32_t warmup_bars = 20;                  // hard floor; features may need more (ADX ready at 1+2*period bars)
    uint32_t vol_window_bars = 20;              // realized-vol / volume-stat window
    uint32_t percentile_window = 100;           // ATR / realized-vol percentile lookback
    uint32_t min_percentile_samples = 20;
    size_t dmi_period = 14;
    double adx_trend_threshold = 25.0;
    double vol_zscore_stress_threshold = 2.5;
    double vol_zscore_expansion_threshold = 1.5;
    double atr_percentile_range_ceiling = 0.40;
    double vol_percentile_stress_floor = 0.80;
};

struct RegimeFeaturesSnapshot {
    bool ready = false;
    double realized_vol_bps = 0.0;
    double realized_vol_percentile = 0.5;
    double adx = 0.0;
    double atr = 0.0;
    double atr_percentile = 0.5;
    double volume_zscore = 0.0;
    std::optional<double> funding_bps;  // absent until the Block 4 funding feed exists
};

/**
 * @brief Deterministic bar-driven regime classifier.
 *
 * Rule priority (first match wins — safety first: Stress must never lose to
 * Trend when both fire):
 *   1. Stress:       volume z-score >= stress threshold AND realized vol in
 *                    its own high percentile.
 *   2. VolExpansion: volume z-score >= expansion threshold.
 *   3. Trend:        ADX >= trend threshold.
 *   4. Range:        ADX below trend threshold AND ATR percentile below the
 *                    range ceiling.
 *   5. Unknown:      warmup incomplete or no rule fired (ambiguous zone —
 *                    declaring Unknown beats forcing a low-quality label).
 *
 * Confidence is the saturating margin of the decisive metric over its
 * threshold: excess/(1+excess), always in [0, 1).
 *
 * O(percentile_window) per bar (percentile scan); bars arrive at seconds-to-
 * minutes cadence, so this is nowhere near any hot path. Latency is still
 * benchmarked (argentum_regime_benchmark) per project discipline.
 */
class RegimeClassifier {
public:
    explicit RegimeClassifier(RegimeClassifierConfig config);

    /// Feed one CLOSED bar (the return value of BarAggregator::on_tick).
    void on_bar(const Bar& bar);

    /// Optional external input; stored for future rules, unused in v1 logic.
    void on_funding_update(double funding_bps);

    [[nodiscard]] RegimeLabel current_label() const { return label_; }
    [[nodiscard]] double current_confidence() const { return confidence_; }
    [[nodiscard]] const RegimeFeaturesSnapshot& features() const { return snapshot_; }
    [[nodiscard]] uint64_t bars_seen() const { return bars_seen_; }

private:
    [[nodiscard]] bool features_ready() const;
    void classify();

    static double saturating_margin(double excess_ratio);

    RegimeClassifierConfig config_;
    RollingWindowStat returns_stat_;
    RollingWindowStat volume_stat_;
    PercentileWindow atr_percentile_;
    PercentileWindow vol_percentile_;
    WilderDmi dmi_;

    uint64_t bars_seen_ = 0;
    double last_close_ = 0.0;
    RegimeLabel label_ = RegimeLabel::Unknown;
    double confidence_ = 0.0;
    RegimeFeaturesSnapshot snapshot_{};
};

} // namespace argentum::regime
