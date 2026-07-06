#include "regime/regime_classifier.hpp"

#include <algorithm>
#include <cmath>

namespace argentum::regime {

RegimeClassifier::RegimeClassifier(RegimeClassifierConfig config)
    : config_(config),
      returns_stat_(config.vol_window_bars),
      volume_stat_(config.vol_window_bars),
      atr_percentile_(config.percentile_window),
      vol_percentile_(config.percentile_window),
      dmi_(config.dmi_period) {}

void RegimeClassifier::on_bar(const Bar& bar) {
    if (!(bar.close > 0.0)) {
        return;
    }
    ++bars_seen_;

    // Volume z-score is computed against the window EXCLUDING the current bar
    // (a spike should be measured against its past, not dampened by itself).
    double volume_z = 0.0;
    if (volume_stat_.ready()) {
        const double sd = volume_stat_.stddev();
        if (sd > 1e-12) {
            volume_z = (bar.volume - volume_stat_.mean()) / sd;
        }
    }
    volume_stat_.push(bar.volume);

    if (last_close_ > 0.0) {
        const double ret_bps = std::log(bar.close / last_close_) * 10'000.0;
        returns_stat_.push(ret_bps);
        if (returns_stat_.ready()) {
            vol_percentile_.push(returns_stat_.stddev());
        }
    }
    last_close_ = bar.close;

    dmi_.on_bar(bar.high, bar.low, bar.close);
    if (dmi_.atr_ready()) {
        atr_percentile_.push(dmi_.atr());
    }

    snapshot_.realized_vol_bps = returns_stat_.ready() ? returns_stat_.stddev() : 0.0;
    snapshot_.realized_vol_percentile = vol_percentile_.midrank_percentile(snapshot_.realized_vol_bps);
    snapshot_.adx = dmi_.adx();
    snapshot_.atr = dmi_.atr();
    snapshot_.atr_percentile = atr_percentile_.midrank_percentile(snapshot_.atr);
    snapshot_.volume_zscore = volume_z;
    snapshot_.ready = features_ready();

    classify();
}

void RegimeClassifier::on_funding_update(double funding_bps) {
    if (std::isfinite(funding_bps)) {
        snapshot_.funding_bps = funding_bps;
    }
}

bool RegimeClassifier::features_ready() const {
    return bars_seen_ >= config_.warmup_bars &&
           dmi_.ready() &&
           returns_stat_.ready() &&
           volume_stat_.ready() &&
           atr_percentile_.ready(config_.min_percentile_samples) &&
           vol_percentile_.ready(config_.min_percentile_samples);
}

double RegimeClassifier::saturating_margin(double excess_ratio) {
    if (!(excess_ratio > 0.0)) {
        return 0.0;
    }
    return std::clamp(excess_ratio / (1.0 + excess_ratio), 0.0, 1.0);
}

void RegimeClassifier::classify() {
    if (!snapshot_.ready) {
        label_ = RegimeLabel::Unknown;
        confidence_ = 0.0;
        return;
    }

    const double z = snapshot_.volume_zscore;
    const double adx = snapshot_.adx;

    if (z >= config_.vol_zscore_stress_threshold &&
        snapshot_.realized_vol_percentile >= config_.vol_percentile_stress_floor) {
        label_ = RegimeLabel::Stress;
        confidence_ = saturating_margin(
            (z - config_.vol_zscore_stress_threshold) / config_.vol_zscore_stress_threshold);
        return;
    }

    if (z >= config_.vol_zscore_expansion_threshold) {
        label_ = RegimeLabel::VolExpansion;
        confidence_ = saturating_margin(
            (z - config_.vol_zscore_expansion_threshold) / config_.vol_zscore_expansion_threshold);
        return;
    }

    if (adx >= config_.adx_trend_threshold) {
        label_ = RegimeLabel::Trend;
        confidence_ = saturating_margin(
            (adx - config_.adx_trend_threshold) / config_.adx_trend_threshold);
        return;
    }

    if (snapshot_.atr_percentile <= config_.atr_percentile_range_ceiling) {
        label_ = RegimeLabel::Range;
        confidence_ = saturating_margin(
            (config_.atr_percentile_range_ceiling - snapshot_.atr_percentile) /
            config_.atr_percentile_range_ceiling);
        return;
    }

    label_ = RegimeLabel::Unknown;
    confidence_ = 0.0;
}

} // namespace argentum::regime
