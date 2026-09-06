// Clock and trace-context tests (ADR 0014): monotonicity of mono_now_ns,
// ClockCalibration mapping sanity and error bound, TraceSpans first-write-wins
// stamping, and tick id uniqueness/monotonicity.
//
// Uses CHECK (not assert): assert is compiled out in Release builds, and
// CTest runs Release — an assert-based test would be vacuous.

#include "core/time_utils.hpp"
#include "core/trace_context.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <thread>
#include <vector>

#define CHECK(cond) do { if (!(cond)) { std::cerr << "CHECK failed: " << #cond << " line=" << __LINE__ << std::endl; return 1; } } while (0)

int main() {
    using namespace argentum::core;

    // mono_now_ns is monotonic non-decreasing across a tight loop.
    {
        uint64_t previous = mono_now_ns();
        for (int i = 0; i < 100'000; ++i) {
            const uint64_t now = mono_now_ns();
            CHECK(now >= previous);
            previous = now;
        }
    }

    // Calibration: valid, bounded uncertainty, projection tracks wall time.
    {
        const ClockCalibration cal = ClockCalibration::measure(15);
        CHECK(cal.valid());
        // A bracketed read on any sane host is far under 1 ms.
        CHECK(cal.uncertainty_ns < 1'000'000ULL);

        const uint64_t mono_probe = mono_now_ns();
        const uint64_t wall_probe = wall_now_ns();
        const uint64_t projected = cal.wall_at(mono_probe);
        const int64_t error = static_cast<int64_t>(projected) - static_cast<int64_t>(wall_probe);
        // Within 10 ms immediately after calibration (generous: covers CI VMs).
        CHECK(error > -10'000'000LL && error < 10'000'000LL);

        // Backward projection stays consistent too.
        const uint64_t back = cal.wall_at(cal.mono_ns - 1'000'000ULL);
        CHECK(back == cal.wall_ns - 1'000'000ULL);

        const ClockCalibration invalid{};
        CHECK(!invalid.valid());
        CHECK(invalid.wall_at(mono_probe) == 0);
    }

#ifdef ARGENTUM_ENABLE_LATENCY_TRACE
    // TraceSpans: stamps are monotonic in stage order, first write wins.
    {
        TraceSpans spans;
        spans.tick_id = next_tick_id();
        spans.stamp(TraceStage::TickIngress);
        spans.stamp(TraceStage::SignalEvalStart);
        spans.stamp(TraceStage::GateVerdict);

        CHECK(spans.stamped(TraceStage::TickIngress));
        CHECK(spans.stamped(TraceStage::GateVerdict));
        CHECK(!spans.stamped(TraceStage::BusPublish));
        CHECK(spans.at(TraceStage::SignalEvalStart) >= spans.at(TraceStage::TickIngress));
        CHECK(spans.at(TraceStage::GateVerdict) >= spans.at(TraceStage::SignalEvalStart));

        const uint64_t first = spans.at(TraceStage::GateVerdict);
        spans.stamp(TraceStage::GateVerdict);  // second stamp must be a no-op
        CHECK(spans.at(TraceStage::GateVerdict) == first);

        spans.reset();
        CHECK(!spans.stamped(TraceStage::TickIngress));
        CHECK(spans.tick_id == 0);
    }
#endif

    // Tick ids: strictly increasing within a thread, unique across threads.
    {
        const uint64_t a = next_tick_id();
        const uint64_t b = next_tick_id();
        CHECK(b > a);

        constexpr int kThreads = 4;
        constexpr int kPerThread = 25'000;
        std::vector<std::vector<uint64_t>> ids(kThreads);
        std::vector<std::thread> workers;
        workers.reserve(kThreads);
        for (int t = 0; t < kThreads; ++t) {
            workers.emplace_back([&ids, t] {
                ids[t].reserve(kPerThread);
                for (int i = 0; i < kPerThread; ++i) {
                    ids[t].push_back(argentum::core::next_tick_id());
                }
            });
        }
        for (std::thread& w : workers) {
            w.join();
        }

        std::vector<uint64_t> all;
        all.reserve(kThreads * kPerThread);
        for (const auto& chunk : ids) {
            all.insert(all.end(), chunk.begin(), chunk.end());
        }
        std::sort(all.begin(), all.end());
        for (size_t i = 1; i < all.size(); ++i) {
            CHECK(all[i] != all[i - 1]);
        }
    }

    std::printf("time_trace_test: OK\n");
    return 0;
}
