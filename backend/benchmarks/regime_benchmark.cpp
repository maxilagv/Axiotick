// RegimeClassifier::on_bar() latency over 1M deterministic synthetic bars
// (alternating calm/volatile phases so every classifier branch runs), through
// the shared benchmark harness. JSON path: argv[1], default
// benchmarks_out/regime.json.

#include "benchmark/harness.hpp"
#include "regime/bar_aggregator.hpp"
#include "regime/regime_classifier.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
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

int main(int argc, char** argv) {
    constexpr uint64_t kWarmup = 10'000;
    constexpr uint64_t kIterations = 1'000'000;

    const std::vector<argentum::regime::Bar> bars = synthesize_bars(kWarmup + kIterations);
    argentum::regime::RegimeClassifier classifier(argentum::regime::RegimeClassifierConfig{});

    const argentum::core::LatencyReport report = argentum::benchmark::measure_loop(
        kWarmup, kIterations, [&](uint64_t i) { classifier.on_bar(bars[i]); });

    argentum::benchmark::BenchmarkReporter reporter("regime_benchmark");
    argentum::benchmark::BenchmarkCase result;
    result.name = "regime_classifier_on_bar";
    result.warmup_iterations = kWarmup;
    result.iterations = kIterations;
    result.latency = report;
    result.extra.emplace_back("config", "defaults (vol_window=20, percentile_window=100, dmi_period=14)");
    result.extra.emplace_back("final_label", argentum::regime::to_string(classifier.current_label()));
    reporter.add(result);

    reporter.print_human(stdout);
    const char* json_path = (argc > 1) ? argv[1] : "benchmarks_out/regime.json";
    if (!reporter.write_json(json_path)) {
        std::fprintf(stderr, "[regime_benchmark] failed to write %s\n", json_path);
        return 1;
    }
    std::printf("[regime_benchmark] json: %s\n", json_path);
    return 0;
}
