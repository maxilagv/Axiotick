// Latency benchmark for RegimeClassifier::on_bar(), following the same
// methodology as matching_benchmark: per-call nanosecond timing over 1M
// deterministic synthetic bars, reported as p50/p95/p99/p99.9.
//
// Record alongside results (per docs/LATENCY_AND_SCALE_TARGETS.md): CPU,
// memory, OS, power profile, compiler, build type and these constants.

#include "regime/bar_aggregator.hpp"
#include "regime/regime_classifier.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <vector>

namespace {

class Lcg {
public:
    explicit Lcg(uint64_t seed) : state_(seed) {}

    double next_unit() {
        state_ = state_ * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<double>((state_ >> 11) & ((1ULL << 53) - 1)) /
               static_cast<double>(1ULL << 53);
    }

    double next_gaussian() {
        const double u1 = std::max(1e-12, next_unit());
        const double u2 = next_unit();
        return std::sqrt(-2.0 * std::log(u1)) * std::cos(6.283185307179586 * u2);
    }

private:
    uint64_t state_;
};

size_t percentile_index(size_t size, double q) {
    size_t index = static_cast<size_t>(static_cast<double>(size) * q);
    if (index >= size) {
        index = size - 1;
    }
    return index;
}

std::vector<argentum::regime::Bar> synthesize_bars(size_t count) {
    std::vector<argentum::regime::Bar> bars;
    bars.reserve(count);

    Lcg rng(0xC0FFEEULL);
    double close = 50'000.0;
    uint64_t ts = 1'700'000'000'000'000'000ULL;
    constexpr uint64_t kBarNs = 60ULL * 1'000'000'000ULL;

    for (size_t i = 0; i < count; ++i) {
        // Alternate calm and volatile phases so every classifier branch runs.
        const bool volatile_phase = ((i / 500) % 2) == 1;
        const double vol_bps = volatile_phase ? 25.0 : 4.0;
        const double drift_bps = volatile_phase ? 1.5 : 0.1;

        const double ret_bps = drift_bps + vol_bps * rng.next_gaussian();
        const double open = close;
        close = open * (1.0 + ret_bps / 10'000.0);

        argentum::regime::Bar bar{};
        bar.start_ts_ns = ts;
        bar.end_ts_ns = ts + kBarNs;
        bar.open = open;
        bar.close = close;
        bar.high = std::max(open, close) * (1.0 + (vol_bps / 3.0) / 10'000.0 * rng.next_unit());
        bar.low = std::min(open, close) * (1.0 - (vol_bps / 3.0) / 10'000.0 * rng.next_unit());
        bar.volume = (volatile_phase ? 40.0 : 10.0) * (0.5 + rng.next_unit());
        bar.tick_count = 100;
        bars.push_back(bar);
        ts += kBarNs;
    }

    return bars;
}

} // namespace

int main() {
    constexpr size_t kIterations = 1'000'000;

    const std::vector<argentum::regime::Bar> bars = synthesize_bars(kIterations);
    argentum::regime::RegimeClassifier classifier(argentum::regime::RegimeClassifierConfig{});

    std::vector<uint64_t> latencies_ns;
    latencies_ns.reserve(kIterations);

    for (const auto& bar : bars) {
        const auto start = std::chrono::high_resolution_clock::now();
        classifier.on_bar(bar);
        const auto end = std::chrono::high_resolution_clock::now();
        latencies_ns.push_back(static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count()));
    }

    std::sort(latencies_ns.begin(), latencies_ns.end());
    const double p50 = static_cast<double>(latencies_ns[percentile_index(latencies_ns.size(), 0.50)]);
    const double p95 = static_cast<double>(latencies_ns[percentile_index(latencies_ns.size(), 0.95)]);
    const double p99 = static_cast<double>(latencies_ns[percentile_index(latencies_ns.size(), 0.99)]);
    const double p999 = static_cast<double>(latencies_ns[percentile_index(latencies_ns.size(), 0.999)]);

    std::cout << "[Regime Benchmark] Iterations: " << kIterations << "\n";
    std::cout << "[Regime Benchmark] Config: defaults (vol_window=20, percentile_window=100, dmi_period=14)\n";
    std::cout << "[Regime Benchmark] Final label: "
              << argentum::regime::to_string(classifier.current_label())
              << " confidence=" << classifier.current_confidence() << "\n";
    std::cout << "[Regime Benchmark] P50: " << p50 << " ns\n";
    std::cout << "[Regime Benchmark] P95: " << p95 << " ns\n";
    std::cout << "[Regime Benchmark] P99: " << p99 << " ns\n";
    std::cout << "[Regime Benchmark] P99.9: " << p999 << " ns\n";

    return 0;
}
