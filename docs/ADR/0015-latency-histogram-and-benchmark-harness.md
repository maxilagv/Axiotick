# ADR 0015: Shared Latency Histogram and Benchmark Harness v2

## Status

Accepted (implemented in Latency Block 1, July 2026). Plan: [`docs/PLAN_WIRE_TO_WIRE_LATENCY.md`](../PLAN_WIRE_TO_WIRE_LATENCY.md); companion: [ADR 0014](0014-clock-discipline-and-trace-context.md).

## Context

Percentile computation existed in six duplicated sort-based sites (three benchmarks, the deleted `LatencyTester`, and two runtime paths in `SignalEngine` and `AsyncEventJournal`). The runtime sites retained raw samples in capped vectors and evicted oldest on overflow — FIFO bias that silently skews percentiles under sustained load, plus an O(n log n) sort per snapshot under a lock. Benchmarks additionally: used a different clock (ADR 0014), had no warmup, printed unstructured text, and their environment tables in `docs/benchmarks/` were typed by hand. The `argentum_node` binary shipped a mock benchmark timing a `volatile` multiply.

## Decision

**One histogram: `core::BasicLatencyHistogram<Counter>`** (`latency_histogram.hpp`), HDR-style log-linear:

- Values 0-31 ns exact; above that, each power-of-two range splits into 32 linear sub-buckets → relative quantization error ≤ 1/32 (~3.2%). 1216 fixed counters (~9.5 KiB), zero allocation, zero locks; `record()` is a few ALU ops plus one increment.
- Percentiles report the bucket **upper edge** (a latency claim may round up, never down), clamped to the exact observed min/max; a percentile landing in the top (overflow) bucket reports the exact observed max rather than under-reporting a clamped value. `report()` returns the canonical `samples/min/p50/p95/p99/p99.9/max/mean` block.
- Two instantiations: `LatencyHistogram` (plain, single-writer) and `ConcurrentLatencyHistogram` (relaxed atomics, multi-producer — used by the per-topic bus publish histogram; live concurrent reads are monitoring-grade by contract).
- Reference-tested against exact sorted-vector percentiles on uniform, bimodal and heavy-tail distributions with asserted error bounds, plus merge-equivalence, overflow and concurrency tests (`latency_histogram_test`, CHECK-based because CTest runs Release where `assert` vanishes).

All six duplicated sites migrated; `GateLatencySnapshot`/`JournalLatencySnapshot` keep their shape (plus a new `p999_ns`) with histogram internals; `TopicMetrics` gains publish p50/p95/p99/p99.9.

**One harness: `benchmark::Harness`** (`benchmark/harness.hpp`), used by all four benchmarks (`matching`, `regime`, `pipeline`, and the new `risk` benchmark that Fase 01 promised but never had):

- Explicit warmup excluded from statistics; all timing via `core::mono_now_ns()`.
- JSON output whose environment metadata is **captured programmatically** — CPU brand via cpuid, hardware threads, RAM, OS build via `RtlGetVersion`/`uname`, compiler version macros, build config, timestamp, and the clock-calibration uncertainty of the run. The tables in `docs/benchmarks/` are pasted from tool output, never typed.
- `scripts/check_benchmarks.py` compares result JSON against `docs/benchmarks/thresholds.json` (per-case ns ceilings, initially ~2× baseline) and fails on regression; uncovered cases warn instead of failing so new benchmarks can land before their baseline. CI job is manual-trigger until thresholds are recalibrated for shared-runner noise.

## Consequences

- Runtime percentile numbers shifted on re-measurement: FIFO-eviction bias is gone, quantization (≤3.2%, pessimistic) is in, and benchmarks now warm up. The July 2026 baseline document gets a dated appended section explaining exactly that; the old section is preserved, never overwritten.
- Percentile queries are O(bucket count) with no allocation and no sort; runtime snapshot cost no longer grows with sample count, and snapshots no longer hold a lock through a sort.
- Reported percentile values are bucket upper edges — e.g. a true 200 ns p95 reports as 203. This is deliberate: claims stay pessimistic and the error bound is documented and tested.
- The mock `LatencyTester` is deleted; `argentum_node` no longer prints fake latency numbers.
- The overhead of the ADR-0014 instrumentation itself is measured by building with `ARGENTUM_ENABLE_LATENCY_TRACE=OFF` vs ON and comparing `argentum_pipeline_benchmark` — the acceptance evidence lives in the baseline document.
