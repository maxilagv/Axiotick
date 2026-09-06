#pragma once

// HDR-style log-linear latency histogram (ADR 0015).
//
// One histogram implementation for every percentile in the codebase — runtime
// snapshots and benchmarks alike — replacing the previous six duplicated
// sort-based sites, two of which evicted oldest samples (FIFO bias) under
// sustained load.
//
// Layout: values 0..31 ns map to exact buckets; above that, each power-of-two
// range is split into 32 linear sub-buckets, so the relative quantization
// error is bounded by 1/32 (~3.2%). Reported percentiles use the bucket's
// upper edge (pessimistic — a latency claim can round up, never down) and are
// clamped to the exact observed min/max. Values above ~73 minutes clamp into
// the top bucket; the true max is still tracked exactly.
//
// Memory: fixed 1216 counters (~9.5 KiB with 64-bit counts), no allocation
// after construction, no locks. record() is a handful of ALU ops plus one
// counter increment — hot-path safe under the rules in
// docs/LATENCY_AND_SCALE_TARGETS.md.
//
// Threading: LatencyHistogram is single-writer (or externally synchronized).
// ConcurrentLatencyHistogram uses relaxed atomics so multiple producers may
// record concurrently; percentile reads on a live concurrent histogram are
// approximate (monitoring-grade), exact once writers quiesce.

#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace argentum::core {

/// Canonical percentile block required by docs/LATENCY_AND_SCALE_TARGETS.md.
struct LatencyReport {
    uint64_t samples = 0;
    uint64_t min_ns = 0;
    uint64_t p50_ns = 0;
    uint64_t p95_ns = 0;
    uint64_t p99_ns = 0;
    uint64_t p999_ns = 0;
    uint64_t max_ns = 0;
    double mean_ns = 0.0;
};

template <typename Counter>
class BasicLatencyHistogram {
public:
    static constexpr int kSubBucketBits = 5;
    static constexpr uint64_t kSubBuckets = 1ULL << kSubBucketBits;      // 32
    static constexpr int kMaxExponent = 41;                              // top group [2^41, 2^42)
    static constexpr size_t kGroupCount = static_cast<size_t>(kMaxExponent - kSubBucketBits + 2);
    static constexpr size_t kBucketCount = kGroupCount * kSubBuckets;    // 1216
    static constexpr uint64_t kMaxTrackedValue = (1ULL << (kMaxExponent + 1)) - 1;

    void record(uint64_t value_ns) noexcept {
        increment(buckets_[bucket_index(value_ns)]);
        increment(count_);
        add(sum_, value_ns);
        update_max(value_ns);
        update_min(value_ns);
    }

    [[nodiscard]] uint64_t count() const noexcept { return load(count_); }
    [[nodiscard]] uint64_t max() const noexcept { return load(max_); }

    [[nodiscard]] uint64_t min() const noexcept {
        const uint64_t raw = load(min_);
        return (raw == UINT64_MAX) ? 0 : raw;
    }

    [[nodiscard]] double mean() const noexcept {
        const uint64_t n = count();
        if (n == 0) return 0.0;
        return static_cast<double>(load(sum_)) / static_cast<double>(n);
    }

    /// q in [0.0, 1.0]. Returns the pessimistic (upper-edge) estimate of the
    /// q-quantile, clamped to the exact observed [min, max].
    [[nodiscard]] uint64_t percentile(double q) const noexcept {
        const uint64_t n = count();
        if (n == 0) return 0;
        if (q < 0.0) q = 0.0;
        if (q > 1.0) q = 1.0;

        uint64_t rank = static_cast<uint64_t>(q * static_cast<double>(n) + 0.5);
        if (rank == 0) rank = 1;
        if (rank > n) rank = n;

        uint64_t cumulative = 0;
        for (size_t i = 0; i < kBucketCount; ++i) {
            cumulative += load(buckets_[i]);
            if (cumulative >= rank) {
                if (i == kBucketCount - 1) {
                    // Top bucket also holds clamped overflow values; the
                    // bucket edge would under-report them. The exact observed
                    // max is the honest (and still pessimistic) answer.
                    return max();
                }
                return clamp_to_observed(bucket_upper_edge(i));
            }
        }
        return max();
    }

    [[nodiscard]] LatencyReport report() const noexcept {
        LatencyReport out{};
        out.samples = count();
        if (out.samples == 0) return out;
        out.min_ns = min();
        out.p50_ns = percentile(0.50);
        out.p95_ns = percentile(0.95);
        out.p99_ns = percentile(0.99);
        out.p999_ns = percentile(0.999);
        out.max_ns = max();
        out.mean_ns = mean();
        return out;
    }

    void reset() noexcept {
        for (auto& bucket : buckets_) store(bucket, 0);
        store(count_, 0);
        store(sum_, 0);
        store(max_, 0);
        store(min_, UINT64_MAX);
    }

    /// Merges another histogram into this one (plain variant only — merging
    /// while writers are active on either side is a data race by contract).
    void merge(const BasicLatencyHistogram& other) noexcept {
        for (size_t i = 0; i < kBucketCount; ++i) {
            add(buckets_[i], load(other.buckets_[i]));
        }
        add(count_, load(other.count_));
        add(sum_, load(other.sum_));
        update_max(load(other.max_));
        const uint64_t other_min = load(other.min_);
        if (other_min != UINT64_MAX) update_min(other_min);
    }

    /// Exact bucket math, exposed for tests: index for a value and the
    /// pessimistic representative (upper edge) of a bucket.
    static constexpr size_t bucket_index(uint64_t value) noexcept {
        if (value > kMaxTrackedValue) value = kMaxTrackedValue;
        if (value < kSubBuckets) return static_cast<size_t>(value);
        const int msb = 63 - std::countl_zero(value);
        const int shift = msb - kSubBucketBits;
        const uint64_t sub = (value >> shift) & (kSubBuckets - 1);
        const size_t group = static_cast<size_t>(msb - kSubBucketBits + 1);
        return group * kSubBuckets + static_cast<size_t>(sub);
    }

    static constexpr uint64_t bucket_upper_edge(size_t index) noexcept {
        if (index < kSubBuckets) return static_cast<uint64_t>(index);
        const size_t group = index / kSubBuckets;
        const uint64_t sub = static_cast<uint64_t>(index % kSubBuckets);
        const int msb = static_cast<int>(group) + kSubBucketBits - 1;
        const uint64_t base = 1ULL << msb;
        const uint64_t width = base >> kSubBucketBits;
        return base + sub * width + (width - 1);
    }

private:
    static constexpr bool kAtomic = !std::is_same_v<Counter, uint64_t>;

    static uint64_t load(const Counter& c) noexcept {
        if constexpr (kAtomic) {
            return c.load(std::memory_order_relaxed);
        } else {
            return c;
        }
    }

    static void store(Counter& c, uint64_t v) noexcept {
        if constexpr (kAtomic) {
            c.store(v, std::memory_order_relaxed);
        } else {
            c = v;
        }
    }

    static void increment(Counter& c) noexcept { add(c, 1); }

    static void add(Counter& c, uint64_t v) noexcept {
        if constexpr (kAtomic) {
            c.fetch_add(v, std::memory_order_relaxed);
        } else {
            c += v;
        }
    }

    void update_max(uint64_t v) noexcept {
        if constexpr (kAtomic) {
            uint64_t prev = max_.load(std::memory_order_relaxed);
            while (v > prev &&
                   !max_.compare_exchange_weak(prev, v, std::memory_order_relaxed,
                                               std::memory_order_relaxed)) {
            }
        } else {
            if (v > max_) max_ = v;
        }
    }

    void update_min(uint64_t v) noexcept {
        if constexpr (kAtomic) {
            uint64_t prev = min_.load(std::memory_order_relaxed);
            while (v < prev &&
                   !min_.compare_exchange_weak(prev, v, std::memory_order_relaxed,
                                               std::memory_order_relaxed)) {
            }
        } else {
            if (v < min_) min_ = v;
        }
    }

    [[nodiscard]] uint64_t clamp_to_observed(uint64_t v) const noexcept {
        const uint64_t hi = max();
        const uint64_t lo = min();
        if (v > hi) return hi;
        if (v < lo) return lo;
        return v;
    }

    std::array<Counter, kBucketCount> buckets_{};
    Counter count_{};
    Counter sum_{};
    Counter max_{};
    Counter min_{init_min()};

    static constexpr uint64_t init_min() noexcept { return UINT64_MAX; }
};

using LatencyHistogram = BasicLatencyHistogram<uint64_t>;
using ConcurrentLatencyHistogram = BasicLatencyHistogram<std::atomic<uint64_t>>;

// Exact-bucket sanity, checked at compile time: the linear region is exact
// and the log-linear region contains its own boundaries.
static_assert(LatencyHistogram::bucket_index(0) == 0);
static_assert(LatencyHistogram::bucket_index(31) == 31);
static_assert(LatencyHistogram::bucket_index(32) == 32);
static_assert(LatencyHistogram::bucket_upper_edge(LatencyHistogram::bucket_index(31)) == 31);
static_assert(LatencyHistogram::bucket_upper_edge(LatencyHistogram::bucket_index(32)) == 32);
static_assert(LatencyHistogram::bucket_upper_edge(LatencyHistogram::bucket_index(1000)) >= 1000);
static_assert(LatencyHistogram::bucket_upper_edge(LatencyHistogram::bucket_index(1000)) <= 1031);

} // namespace argentum::core
