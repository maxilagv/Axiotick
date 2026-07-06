#pragma once

#include <algorithm>
#include <cstddef>
#include <vector>

namespace argentum::risk {

/**
 * @brief Historical (empirical) VaR/CVaR. Both values use the positive-loss
 * convention: VaR 95 = 12.5 means "the 5% worst outcome loses 12.5 or more".
 *
 * Empirical method on the sorted sample — no distributional assumption, which
 * is the point: crypto/equity strategy returns are fat-tailed and the
 * parametric (normal) method in var_calculator.hpp understates tail risk.
 * Percentile convention matches the rest of the codebase: sorted index
 * floor(q * (n-1)), no interpolation. CVaR is the mean of the tail up to and
 * including that index.
 */
struct HistoricalVarResult {
    double var = 0.0;
    double cvar = 0.0;
};

[[nodiscard]] inline HistoricalVarResult historical_var_cvar(
    std::vector<double> returns,
    double confidence_level) {
    HistoricalVarResult result{};
    if (returns.empty() || !(confidence_level > 0.0) || !(confidence_level < 1.0)) {
        return result;
    }

    std::sort(returns.begin(), returns.end());
    const size_t n = returns.size();
    const size_t tail_index =
        static_cast<size_t>((1.0 - confidence_level) * static_cast<double>(n - 1));

    result.var = std::max(0.0, -returns[tail_index]);

    double tail_sum = 0.0;
    for (size_t i = 0; i <= tail_index; ++i) {
        tail_sum += returns[i];
    }
    result.cvar = std::max(0.0, -(tail_sum / static_cast<double>(tail_index + 1)));
    return result;
}

} // namespace argentum::risk
