// Reference tests for core::LatencyHistogram (ADR 0015): percentile accuracy
// against exact sorted-vector percentiles on uniform, bimodal and heavy-tail
// distributions with asserted error bounds; merge associativity; overflow
// clamping; and a concurrent-variant smoke test.
//
// Uses CHECK (not assert): assert is compiled out in Release builds, and
// CTest runs Release — an assert-based test would be vacuous.

#include "core/latency_histogram.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <thread>
#include <vector>

#define CHECK(cond) do { if (!(cond)) { std::cerr << "CHECK failed: " << #cond << " line=" << __LINE__ << std::endl; return 1; } } while (0)

namespace {

using argentum::core::ConcurrentLatencyHistogram;
using argentum::core::LatencyHistogram;

class Lcg {
public:
    explicit Lcg(uint64_t seed) : state_(seed) {}

    uint64_t next() {
        state_ = state_ * 6364136223846793005ULL + 1442695040888963407ULL;
        return state_ >> 11;
    }

    double next_unit() {
        return static_cast<double>(next() & ((1ULL << 53) - 1)) / static_cast<double>(1ULL << 53);
    }

private:
    uint64_t state_;
};

uint64_t exact_percentile(std::vector<uint64_t> values, double q) {
    std::sort(values.begin(), values.end());
    uint64_t rank = static_cast<uint64_t>(q * static_cast<double>(values.size()) + 0.5);
    if (rank == 0) rank = 1;
    if (rank > values.size()) rank = values.size();
    return values[rank - 1];
}

// The histogram reports the bucket upper edge, so it may sit up to one bucket
// width above the exact order statistic (pessimistic by design). Bound both
// directions with the 1/32 relative error plus small absolute slack for the
// sub-32ns region.
bool close_enough(uint64_t approx, uint64_t exact, const char* label) {
    const double bound = static_cast<double>(exact) / 32.0 + 2.0;
    const double diff = static_cast<double>(approx > exact ? approx - exact : exact - approx);
    if (diff > bound) {
        std::fprintf(stderr, "FAIL %s: approx=%llu exact=%llu bound=%.1f\n", label,
                     static_cast<unsigned long long>(approx),
                     static_cast<unsigned long long>(exact), bound);
        return false;
    }
    return true;
}

bool distribution_case_ok(const std::vector<uint64_t>& values, const char* label) {
    LatencyHistogram hist;
    for (const uint64_t v : values) {
        hist.record(v);
    }

    if (hist.count() != values.size()) return false;
    if (hist.min() != *std::min_element(values.begin(), values.end())) return false;
    if (hist.max() != *std::max_element(values.begin(), values.end())) return false;

    for (const double q : {0.50, 0.95, 0.99, 0.999}) {
        if (!close_enough(hist.percentile(q), exact_percentile(values, q), label)) {
            return false;
        }
    }
    return true;
}

} // namespace

int main() {
    // Exact in the linear region: values below 32 have zero quantization error.
    {
        LatencyHistogram hist;
        for (uint64_t v = 0; v < 32; ++v) {
            hist.record(v);
        }
        CHECK(hist.percentile(0.5) == 15);  // rank 16 of 32 lands on value 15, exact here
        CHECK(hist.min() == 0);
        CHECK(hist.max() == 31);
    }

    // Empty histogram is all zeros, no crashes.
    {
        LatencyHistogram hist;
        CHECK(hist.count() == 0);
        CHECK(hist.percentile(0.99) == 0);
        CHECK(hist.report().samples == 0);
    }

    Lcg rng(0xA110C57ULL);

    // Uniform 50..5000 ns.
    {
        std::vector<uint64_t> values;
        values.reserve(200'000);
        for (size_t i = 0; i < 200'000; ++i) {
            values.push_back(50 + static_cast<uint64_t>(rng.next_unit() * 4950.0));
        }
        CHECK(distribution_case_ok(values, "uniform"));
    }

    // Bimodal: fast path ~100ns, slow path ~50us (scheduler-noise shape).
    {
        std::vector<uint64_t> values;
        values.reserve(200'000);
        for (size_t i = 0; i < 200'000; ++i) {
            if (rng.next_unit() < 0.95) {
                values.push_back(80 + static_cast<uint64_t>(rng.next_unit() * 60.0));
            } else {
                values.push_back(45'000 + static_cast<uint64_t>(rng.next_unit() * 15'000.0));
            }
        }
        CHECK(distribution_case_ok(values, "bimodal"));
    }

    // Heavy tail: spread over four decades.
    {
        std::vector<uint64_t> values;
        values.reserve(200'000);
        for (size_t i = 0; i < 200'000; ++i) {
            const double u = rng.next_unit();
            values.push_back(100 + static_cast<uint64_t>(u * u * u * 1'000'000.0));
        }
        CHECK(distribution_case_ok(values, "heavy_tail"));
    }

    // Overflow clamping: huge values land in the top bucket; percentile stays
    // clamped to the exact observed max.
    {
        LatencyHistogram hist;
        hist.record(UINT64_MAX);
        hist.record(UINT64_MAX - 5);
        hist.record(100);
        CHECK(hist.max() == UINT64_MAX);
        CHECK(hist.percentile(0.999) == UINT64_MAX);
        CHECK(hist.min() == 100);
    }

    // Merge: (A merge B) reports identically to recording everything into one.
    {
        LatencyHistogram a;
        LatencyHistogram b;
        LatencyHistogram all;
        for (size_t i = 0; i < 50'000; ++i) {
            const uint64_t v1 = 100 + static_cast<uint64_t>(rng.next_unit() * 900.0);
            const uint64_t v2 = 10'000 + static_cast<uint64_t>(rng.next_unit() * 90'000.0);
            a.record(v1);
            b.record(v2);
            all.record(v1);
            all.record(v2);
        }
        a.merge(b);
        CHECK(a.count() == all.count());
        CHECK(a.min() == all.min());
        CHECK(a.max() == all.max());
        for (const double q : {0.50, 0.95, 0.99, 0.999}) {
            CHECK(a.percentile(q) == all.percentile(q));
        }
    }

    // Concurrent variant smoke: N producers record simultaneously; totals and
    // range survive, percentile sits inside the recorded envelope.
    {
        ConcurrentLatencyHistogram hist;
        constexpr int kThreads = 4;
        constexpr uint64_t kPerThread = 100'000;
        std::vector<std::thread> workers;
        workers.reserve(kThreads);
        for (int t = 0; t < kThreads; ++t) {
            workers.emplace_back([&hist, t] {
                for (uint64_t i = 0; i < kPerThread; ++i) {
                    hist.record(100 + (static_cast<uint64_t>(t) * 1000) + (i % 500));
                }
            });
        }
        for (std::thread& w : workers) {
            w.join();
        }
        CHECK(hist.count() == kThreads * kPerThread);
        CHECK(hist.min() >= 100);
        CHECK(hist.max() <= 100 + 3000 + 499);
        const uint64_t p50 = hist.percentile(0.50);
        CHECK(p50 >= hist.min() && p50 <= hist.max());
    }

    // reset() really zeroes everything.
    {
        LatencyHistogram hist;
        hist.record(12345);
        hist.reset();
        CHECK(hist.count() == 0);
        CHECK(hist.max() == 0);
        CHECK(hist.percentile(0.5) == 0);
    }

    std::printf("latency_histogram_test: OK\n");
    return 0;
}
