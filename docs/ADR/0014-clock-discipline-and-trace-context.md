# ADR 0014: Clock Discipline and Wire-to-Wire Trace Context

## Status

Accepted (implemented in Latency Block 1, July 2026). Plan: [`docs/PLAN_WIRE_TO_WIRE_LATENCY.md`](../PLAN_WIRE_TO_WIRE_LATENCY.md); thesis: [`docs/DIFFERENTIATION_THESIS.md`](../DIFFERENTIATION_THESIS.md) capability C.

## Context

The July 2026 audit found the codebase could not verify a single wire-to-wire latency claim:

- Every event struct carried exactly one `timestamp_ns`; there was no ingress-vs-source time distinction, and the market gateway did not stamp receive time at all.
- Benchmarks timed with `high_resolution_clock` while runtime code used `core::now_ns()` (steady) — two clock bases whose numbers were silently non-comparable.
- Correlation was one hand-threaded `related_signal_id`; ticks had no id, and cancel/replace journal events dropped the signal id entirely (journaled as 0), so a canceled order could not be attributed to its originating decision.
- Nothing measured the relationship between the monotonic and wall clocks, making any future markout (Block 2) unanchorable.

## Decision

**One clock module, two explicit domains.** `core::mono_now_ns()` (durations only) and `core::wall_now_ns()` (event stamps only) are the only time sources. The old names `now_ns`/`unix_now_ns` remain as deprecated inline aliases. `time_utils.cpp` is the single translation unit allowed to touch `std::chrono` clocks; `scripts/check_clock_discipline.py` enforces this in CI, and bans `high_resolution_clock` outright. `core::ClockCalibration` measures the mono→wall mapping with a bracketed read (uncertainty = half the tightest bracket) so monotonic spans can be projected onto wall time with a known error bound; `argentum_clock_check` reports drift between calibrations.

**Ingress timestamp on market data.** `MarketTick` gains `ingress_ns` (wall clock, first instant this process owned the tick; 0 = never stamped) placed in the trailing alignment padding at offset 56. Layout consequences are pinned by `static_assert`s: size stays 64, `side` stays at offset 48, so legacy V1 memcpy payloads decode with `ingress_ns == 0` and every old field intact. The FlatBuffers table appends the field (absent → 0). Stamping rule: the point that mints or first receives the tick stamps it; downstream never overwrites.

**Trace context.** `core::TraceSpans` — a stack-allocated POD carrying `tick_id` (process-monotonic counter minted at ingress, no UUIDs) plus one first-write-wins monotonic stamp per pipeline stage (`TickIngress, Decode, BusPublish, BusConsume, BarClose, SignalEvalStart, GateVerdict, RiskVerdict, OmsAccept, JournalEnqueue`). It flows by pointer through the synchronous decision chain: `SignalEngine::process(candidate, trace)` → `OrderManager::submit_order(order, signal_id, trace)`. `SignalCandidate` gains `origin_tick_id`. The id chain journaled per decision is now tick_id → signal_id → order events.

**Correlation bug fix.** `OrderState` remembers `related_signal_id` and `origin_tick_id` for the order's whole lifecycle; cancel, partial-cancel, modify and replace journal events now carry both (they used to emit 0). Regression-tested in `trace_correlation_journal_test`.

**Aggregation and reporting.** `core::PipelineTelemetry` absorbs spans off the hot path into per-stage histograms plus `wire→gate` and `wire→decision` (ingress to OMS accept, or to gate verdict on rejects). The demos and Mode B backtests print the report and emit JSON; `StrategyBacktestReport` carries it, which is meaningful precisely because Mode B executes the exact production decision path. Full per-stage spans go to the `signal_decision` audit event for every reject and 1-in-N accepts (`trace_audit_sample_every`, default 64) — tails stay observable without inflating the audit log.

**Build flag.** `ARGENTUM_ENABLE_LATENCY_TRACE` (default ON) compiles `TraceSpans::stamp()` to nothing when OFF; the OFF-vs-ON delta on `argentum_pipeline_benchmark` is the measured instrumentation overhead (Block 1 acceptance gate: < 2% throughput, < 50 ns p99 per stamp).

## Consequences

- Every latency number in the repo now shares one clock base, and any decision can be traced tick → signal → order → cancel with ids in the journal. Old journals (no `tick_id`) replay unchanged — the parser treats the field as optional.
- The journal's hybrid wall/monotonic `timestamp_ns` normalization (ADR 0008 behavior) stands; monotonic span data now travels in the audit event instead of overloading the journal timestamp, so the two domains are never conflated again.
- Rate-limit windows in the API layer migrated from `steady_clock` time_points to mono-ns arithmetic; the mock `LatencyTester` (which timed a volatile multiply) is deleted from `argentum_node`.
- The wall-clock quality itself is an operational requirement, not code: production needs NTP with measured offset < 1 ms (PTP target on Linux later), now a live-readiness checklist item with `argentum_clock_check` as the measurement tool.
- The bus-thread path (`BusPublish`/`BusConsume`/`Decode` stages) is stamped only where the tick physically crosses the bus; the demo decision chains are synchronous single-thread today, so those stages report empty there — honest, and ready for the gateway→engine wiring when paper trading lands.
