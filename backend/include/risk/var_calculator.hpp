#pragma once

#include <vector>
#include <cmath>
#include <numeric>

namespace argentum::risk {

/**
 * @class VaRCalculator
 * @brief Computes Value at Risk using Parametric method (Variance-Covariance).
 */
class VaRCalculator {
public:
    /**
     * @brief Calculate Daily VaR.
     * @param returns Vector of historical percentage returns.
     * @param confidence_level e.g., 0.95 or 0.99.
     * @param portfolio_value Current value of portfolio.
     * @return The maximum expected loss.
     */
    static double calculate_parametric_var(const std::vector<double>& returns, 
                                           double confidence_level, 
                                           double portfolio_value) {
        if (returns.empty()) return 0.0;

        // 1. Calculate Mean
        double sum = std::accumulate(returns.begin(), returns.end(), 0.0);
        double mean = sum / returns.size();

        // 2. Calculate Standard Deviation (Volatility)
        double sq_sum = std::inner_product(returns.begin(), returns.end(), returns.begin(), 0.0);
        double stdev = std::sqrt(sq_sum / returns.size() - mean * mean);

        // 3. Z-Score (Approximate for Normal Distribution)
        double z_score = 0.0;
        if (confidence_level == 0.95) z_score = 1.65;
        else if (confidence_level == 0.99) z_score = 2.33;
        else z_score = 1.96; // default 97.5%

        return portfolio_value * z_score * stdev;
    }
};

} // namespace argentum::risk
