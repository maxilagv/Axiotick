#pragma once

// Wire-to-wire trace context (ADR 0014).
//
// A TraceSpans travels with one market tick through the synchronous decision
// chain (tick -> bar -> signal -> gate -> risk -> OMS -> journal) collecting
// one monotonic stamp per stage boundary. It is a fixed-size POD on the
// caller's stack: no allocation, no locks, no I/O. When the build flag
// ARGENTUM_ENABLE_LATENCY_TRACE is OFF, stamp() compiles to nothing — the
// OFF-vs-ON delta on argentum_pipeline_benchmark is the measured overhead of
// the instrumentation itself.
//
// tick_id is a per-process monotonic id minted at ingress (no UUIDs on the
// hot path). It joins the three records of one decision: the tick, the
// signal_decision audit event, and every journal event of the resulting
// order — including cancels and replaces.

#include "core/time_utils.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace argentum::core {

// Stage indices follow the PHYSICAL order of the accepted-order flow —
// PipelineTelemetry::absorb() computes stage-to-stage deltas in index order,
// so an out-of-order index silently drops its stage's samples. In the OMS the
// first journal event is enqueued while the order is being processed, BEFORE
// the final accept verdict; hence JournalEnqueue < OmsAccept.
enum class TraceStage : uint8_t {
    TickIngress = 0,    // first instant this process owned the tick
    Decode = 1,         // codec decode complete (bus paths)
    BusPublish = 2,     // enqueue into the bus ring
    BusConsume = 3,     // dequeue by the consumer thread
    BarClose = 4,       // BarAggregator closed a bar (only on bar boundaries)
    SignalEvalStart = 5,
    GateVerdict = 6,    // EV gate accept/reject decided
    RiskVerdict = 7,    // RiskManager::check_order returned
    JournalEnqueue = 8, // first journal event of the decision enqueued
    OmsAccept = 9       // OMS accept verdict issued (absent on rejects)
};

constexpr size_t kTraceStageCount = 10;

const char* to_string(TraceStage stage);

/// Process-wide monotonic tick id. Relaxed atomic increment; ids are unique
/// within a process run, not across runs (replay correlates via the journal).
uint64_t next_tick_id();

struct TraceSpans {
    uint64_t tick_id = 0;
    uint64_t ingress_wall_ns = 0;  // wall-clock twin of the TickIngress stamp
    std::array<uint64_t, kTraceStageCount> mono_ns{};

    /// First write wins: a stage that fires twice for one tick (e.g. several
    /// journal events per order) keeps its earliest boundary crossing.
    void stamp(TraceStage stage) noexcept {
#ifdef ARGENTUM_ENABLE_LATENCY_TRACE
        uint64_t& slot = mono_ns[static_cast<size_t>(stage)];
        if (slot == 0) {
            slot = mono_now_ns();
        }
#else
        (void)stage;
#endif
    }

    [[nodiscard]] uint64_t at(TraceStage stage) const noexcept {
        return mono_ns[static_cast<size_t>(stage)];
    }

    [[nodiscard]] bool stamped(TraceStage stage) const noexcept {
        return at(stage) != 0;
    }

    void reset() noexcept {
        tick_id = 0;
        ingress_wall_ns = 0;
        mono_ns.fill(0);
    }
};

} // namespace argentum::core
