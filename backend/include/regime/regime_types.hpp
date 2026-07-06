#pragma once

#include <cstddef>
#include <cstdint>

namespace argentum::regime {

/**
 * @brief Market regime labels used to condition signal acceptance and reporting.
 *
 * Block 1 defines the contract only; the classifier that produces these labels
 * (bar aggregation + realized volatility / trend strength / range features)
 * lands in Block 2. Until then producers should emit Unknown with confidence 0.
 */
enum class RegimeLabel : uint8_t {
    Unknown = 0,
    Trend = 1,
    Range = 2,
    Stress = 3,
    VolExpansion = 4
};

inline constexpr size_t kRegimeCount = 5;

[[nodiscard]] inline constexpr uint32_t regime_bit(RegimeLabel label) {
    return 1u << static_cast<uint8_t>(label);
}

inline constexpr uint32_t kAllRegimesMask = (1u << kRegimeCount) - 1;

[[nodiscard]] inline const char* to_string(RegimeLabel label) {
    switch (label) {
        case RegimeLabel::Unknown: return "unknown";
        case RegimeLabel::Trend: return "trend";
        case RegimeLabel::Range: return "range";
        case RegimeLabel::Stress: return "stress";
        case RegimeLabel::VolExpansion: return "vol_expansion";
        default: return "invalid";
    }
}

[[nodiscard]] inline constexpr bool is_valid_regime(RegimeLabel label) {
    return static_cast<uint8_t>(label) < kRegimeCount;
}

} // namespace argentum::regime
