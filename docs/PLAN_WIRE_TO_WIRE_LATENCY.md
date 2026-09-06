# Plan — Block 1: Wire-to-Wire Latency and Clock Discipline

**Status: approved plan, not yet implemented.** This is the execution plan for Block 1 of the [Differentiation Thesis](DIFFERENTIATION_THESIS.md) (capability C). Nothing below exists until its sub-block is delivered and this line is updated per sub-block.

Date: July 2026. Scope: measurement only — no strategy, signal or risk behavior changes. Every hot-path rule in [`LATENCY_AND_SCALE_TARGETS.md`](LATENCY_AND_SCALE_TARGETS.md) applies to the instrumentation itself.

---

## 1. Objective and definition of done

**Objective:** make every latency claim in this repository verifiable wire-to-wire — one disciplined clock architecture, one histogram implementation, one trace context from tick ingress to journaled order, and a benchmark harness whose environment metadata and acceptance thresholds are produced by machines, not typed by hand.

**Definition of done (the whole block):**

1. A single tick can be traced through ingress → decode → publish → consume → bar → signal → gate → risk → OMS → journal with a per-stage timestamp chain, and the demos print a wire-to-decision percentile report (p50/p95/p99/p99.9/max) built from it.
2. All percentile computation in the repo goes through one shared, tested, bounded histogram — zero duplicated sort-based sites remain.
3. All latency measurement (runtime and benchmarks) uses one clock module; direct `std::chrono::high_resolution_clock` use is gone and guarded against by CI.
4. Benchmarks emit machine-readable JSON including programmatically captured environment metadata, run with explicit warmup, and a comparison script fails on regression beyond declared tolerances.
5. Instrumentation overhead is measured and within budget: < 2% throughput impact on `argentum_pipeline_benchmark`, < 50 ns p99 per stage stamp.
6. Deterministic replay is unaffected: journal records with the new fields replay correctly, and records written before this block still parse.

## 2. Current state (audited, with evidence)

Findings from the July 2026 code audit that this plan is built on:

| # | Finding | Evidence |
|---|---|---|
| F1 | Central time utility exists (`core::now_ns()` steady, `core::unix_now_ns()` wall) but benchmarks bypass it with `high_resolution_clock` | `backend/include/core/time_utils.hpp`; `benchmarks/matching_benchmark.cpp:61`, `benchmarks/regime_benchmark.cpp:95`, `include/benchmark/latency_tester.hpp:14` |
| F2 | No ingress vs source timestamp. `MarketTick`/`Order`/`Trade` carry a single `timestamp_ns`; the market gateway does not stamp receive time | `backend/include/core/types.h:55-100`; `market_gateway.cpp:68-74` |
| F3 | Six duplicated sort-based percentile sites; two run at runtime with FIFO-eviction bias (gate latency capped 65536, journal capped 4096) | `matching_benchmark.cpp:30`, `regime_benchmark.cpp:40`, `pipeline_benchmark.cpp:103`, `latency_tester.hpp:42`, `signal_engine.cpp:28,150-174`, `event_journal.cpp:187,313-326` |
| F4 | Bus metrics are per-topic counters + running avg + max only — no percentiles | `message_bus.cpp:190-197,287-301`; `message_bus.hpp:34-41` |
| F5 | Correlation is a single hand-threaded `related_signal_id`; ticks have no id; cancel/replace journal events drop the id (default 0) | `signal_engine.cpp:52,82`; `order_manager.hpp:76`; `order_manager.cpp:252,324,371`; `event_journal.hpp:61` |
| F6 | Benchmark environment metadata is hand-written into `docs/benchmarks/`; binaries print unstructured text; no warmup phase in any benchmark | `regime_benchmark.cpp:4-6`; `docs/benchmarks/2026-07-decision-engine-baseline.md` |
| F7 | Demo decision chain is synchronous single-thread; the only async hops are journal writer and audit logger threads. The bus/thread boundary exists only on the gateway path | `backtest_engine.cpp:244-294`; `signal_engine.cpp:49-82`; `order_manager.cpp:473-479` |
| F8 | Journal `timestamp_ns` is a hybrid: wall-clock, then forced strictly monotonic (`previous+1` on regression) | `event_journal.cpp:339-378` |
| F9 | Exactly one build flag exists (`ARGENTUM_USE_FLATBUFFERS`), providing the pattern for a new instrumentation flag | top-level `CMakeLists.txt:39`; `backend/CMakeLists.txt:28-65` |

Roadmap fit: Fase 01 already owns the benchmark schema / clock discipline promises; Fase 10 owns trace-id propagation and per-component runtime metrics. This block delivers the concrete items of both without re-promising them; their READMEs get a pointer here when the block ships.

## 3. Design decisions

These become **ADR 0014 (clock architecture and trace context)** and **ADR 0015 (histogram and benchmark harness)** when implemented, following the four-section ADR convention (Status / Context / Decision / Consequences).

### D1 — One clock module, two explicit domains

Extend `core/time_utils.hpp` into the single source of time:

- `core::mono_now_ns()` — monotonic, for **durations only** (today's `now_ns()`, renamed with a deprecation alias to keep the diff reviewable).
- `core::wall_now_ns()` — system clock UTC epoch ns, for **event stamps only** (today's `unix_now_ns()`).
- `core::ClockCalibration` — a startup-measured mapping between the two (offset + drift bound), re-sampled periodically off the hot path, so any monotonic span can be projected onto wall time with a known error bound. This is what makes +100 ms markouts (Block 2) meaningful later.

Rules, enforced by a CI grep gate (same spirit as the clang-format job): no `high_resolution_clock` anywhere; no direct `steady_clock`/`system_clock` calls outside `time_utils.cpp` and the two documented exceptions (rate-limit windows in `http_ws_server`/`market_gateway` may migrate opportunistically; `is_fx_market_open()` is calendar logic, not timing).

TSC/QPC fast-clock backend: **deferred, not designed away.** `mono_now_ns()` on MSVC already compiles to QPC (~20-30 ns). A raw-TSC backend is added only if sub-block 1.3's overhead measurement shows stamping cost matters — decided by data, per the thesis.

### D2 — Ingress timestamp on market data

`MarketTick` gains `uint64_t ingress_ns` — stamped with `wall_now_ns()` at the first point the process owns the bytes (`MarketGatewayService::on_market_message`, and the tick sources in demos/backtest so the field is never zero in any mode). `timestamp_ns` keeps its current meaning: source/exchange time.

Layout: current payload is ~49 bytes inside a 64-byte cache-line-aligned struct (`static_assert` at `types.h:103-104`), so an extra 8-byte field fits without breaking the 64-byte guarantee. **Acceptance gate: the existing static_asserts must still pass; if any struct is found full, the ingress stamp moves to the bus `MessageHeaderV3` instead — that fallback is a documented decision point, not an improvisation.** FlatBuffers schema (`argentum.fbs`) gains the same optional field.

### D3 — TraceContext: from one hand-threaded id to a real chain

- `tick_id`: a per-process monotonic id minted at ingress alongside `ingress_ns` (a `uint64` counter — no UUIDs on the hot path).
- `SignalCandidate` gains `origin_tick_id`; `SignalDecision`/audit `signal_decision` events record it, linking decision → the exact tick that caused it.
- Fix F5: `OrderManager::cancel`/`replace` paths propagate `related_signal_id` instead of defaulting to 0. This is a bug fix in the existing correlation, shipped first because Block 2's markouts need cancels attributable.
- `TraceSpans`: a fixed-size array of `(stage_id, mono_ns)` pairs (8 stages × 16 bytes, POD, no allocation) carried by value alongside the tick through the synchronous chain (F7 makes this cheap — one thread, no marshalling), and through the bus envelope on the gateway path.
- Journal: order events gain optional `tick_id` and a sampled `spans` field (see D6 sampling). Replay parser treats both as optional — old JSONL lines keep parsing (definition-of-done #6).

### D4 — One histogram to replace six percentile sites

New `core::LatencyHistogram` (in `argentum_core`, no new library target needed):

- HDR-style log-linear bucketing: power-of-2 major buckets × 32 linear sub-buckets, 1 ns – 100 s range, ≤ 3.2% relative error, ~2 KB fixed memory, POD, zero allocation after construction.
- Single-writer `record(ns)` is a couple of arithmetic ops + one increment — safe inside the hot path budget. Cross-thread reads via snapshot-copy under a seqlock or by merge of per-thread instances; **no locks on the record path**.
- `percentile(q)`, `max()`, `count()`, plus a `report()` producing the canonical p50/p95/p99/p99.9/max block every doc requires.
- Reference-tested against exact sorted-vector percentiles on uniform, bimodal, and heavy-tail distributions with asserted error bounds; plus merge-associativity and overflow-bucket tests.

Migration replaces all six F3 sites. The two runtime sites (`SignalEngine::gate_latency_snapshot`, `AsyncEventJournal::latency_snapshot`) keep their public snapshot structs — only the internals change. **Known consequence to document in the ADR: reported numbers will shift slightly (FIFO-eviction bias removed, bounded bucket error added). The baseline doc gets a dated re-measurement, appended, never overwritten.**

### D5 — Per-stage instrumentation and the wire-to-decision report

Stage stamps (via `TraceSpans`) at: ingress, decode done, bus publish, bus consume, bar close, signal eval start, gate verdict, risk verdict, OMS accept, journal enqueue. Per-stage `LatencyHistogram` instances live in a `PipelineTelemetry` struct owned by the engine wiring (demos, gateway service), aggregated off the hot path.

- Bus (F4): `TopicMetricsInternal` gains an optional publish-latency histogram per topic, enabled by the same flag as below; the existing counters stay untouched.
- Demos and `argentum_node` print the wire-to-decision report at shutdown; the report is also emitted as JSON next to the journal for tooling.
- Build flag `ARGENTUM_ENABLE_LATENCY_TRACE` (default **ON**; pattern per F9). The flag exists to prove the overhead claim (a benchmark run with OFF vs ON is the overhead measurement itself), not to encourage running blind.
- Journal span sampling: full spans journaled for 1-in-N decisions (N configurable, default 64) plus always on rejects/kill-switch events — bounds journal growth while keeping tails observable.

### D6 — Benchmark harness v2

A small `benchmark::Harness` used by all three benchmarks plus a new `argentum_risk_benchmark` (risk check latency is promised by Fase 01 and still unmeasured):

- Explicit warmup phase (excluded from stats) — none of the current benchmarks has one (F6).
- All timing through `core::mono_now_ns()` (kills F1).
- Emits JSON: results (full percentile set, throughput, drops, queue depth) **plus environment metadata captured programmatically** — CPU model/cores (cpuid / registry), RAM, OS build, compiler + flags (compile-time macros), build type, feature flags, power profile where queryable. The hand-written table in `docs/benchmarks/` becomes a paste of machine output.
- `scripts/check_benchmarks.py` (offline tooling, Python allowed): compares a JSON result against `docs/benchmarks/thresholds.json` (per-metric ceiling + tolerance), exit non-zero on regression. CI gets an optional manual-trigger job; thresholds start generous (2× baseline) and tighten as variance data accumulates — a noisy gate that gets ignored is worse than none.

### D7 — Clock error discipline

- `argentum_clock_check` utility: measures steady↔system drift over a run, queries OS sync status (w32tm / chronyc), prints the current clock error bound. Its output becomes part of benchmark metadata and, later, of every markout dataset (Block 2 consumes this).
- Documented requirement (`docs/` + live-readiness checklist item): production runtime requires NTP with measured offset < 1 ms; PTP is the target on Linux for multi-host. Deployment itself is out of scope here — the *requirement and the measurement tool* are in scope.
- F8 stands as designed (journal monotonicity is a feature for replay), but the journal record gains the mono-clock span data via D3, so wall-hybrid vs mono are never conflated again.

## 4. Work breakdown

Four sub-blocks, each independently shippable, tested, and benchmarked before the next starts — same discipline as decision-engine Blocks 1-3.

### Sub-block 1.1 — Foundations: clock + histogram (no behavior change)

1. `time_utils` extension (D1) + `ClockCalibration` + tests (monotonicity, calibration error bound sanity).
2. `LatencyHistogram` + reference tests (D4).
3. Migrate the six percentile sites; delete `benchmark/latency_tester.hpp` (unused by real paths).
4. CI grep gate for banned clock APIs.
5. Re-measure and append gate/journal snapshot numbers to the baseline doc.

*Acceptance:* all 38+ CTest targets green; histogram error bounds asserted in tests; zero `high_resolution_clock` hits in CI grep; baseline doc updated with dated append.

### Sub-block 1.2 — Ingress stamps + trace context

1. `MarketTick.ingress_ns` + `tick_id` (D2/D3), layout static_asserts verified, `.fbs` updated.
2. Gateway + demo/backtest tick sources stamp ingress; `SignalCandidate.origin_tick_id` threaded to audit events.
3. Fix cancel/replace `related_signal_id` propagation (F5) + regression test.
4. Journal optional fields + replay backward-compat test (old JSONL fixture must replay).

*Acceptance:* an integration test asserts, for one synthetic tick, the full id chain tick_id → signal_id → order events in journal + audit; old-format replay test green.

### Sub-block 1.3 — Pipeline spans + wire-to-decision report

1. `TraceSpans` + `PipelineTelemetry` (D5); stamps at the ten stage boundaries; bus per-topic histogram.
2. Wire-to-decision report (stdout + JSON) in both demos and `argentum_node`.
3. Journal span sampling (1-in-N + all rejects).
4. **Overhead measurement:** `argentum_pipeline_benchmark` with `ARGENTUM_ENABLE_LATENCY_TRACE` OFF vs ON — must show < 2% throughput delta, stamp cost < 50 ns p99; results appended to baseline doc.

*Acceptance:* overhead budget met and documented; demo prints a full per-stage percentile table from a live run; hot-path rules audit (no I/O, no locks, no allocation on the record path) noted in the ADR.

### Sub-block 1.4 — Harness v2 + clock check

1. `benchmark::Harness` (warmup, JSON, env metadata) adopted by the three existing benchmarks; new `argentum_risk_benchmark`.
2. `thresholds.json` + `scripts/check_benchmarks.py` + optional CI job.
3. `argentum_clock_check` utility (D7) + docs requirement + live-readiness checklist line.
4. Write ADR 0014 and ADR 0015; update Fase 01 / Fase 10 READMEs to point here; update root README status section.

*Acceptance:* one command reproduces the full baseline as JSON with machine-captured metadata; regression script demonstrably fails on an injected slowdown; ADRs merged.

## 5. Risks

| Risk | Mitigation |
|---|---|
| Struct layout break (64-byte asserts) when adding `ingress_ns` | Static_asserts are the gate; documented fallback to header-level stamp (D2) |
| Instrumentation overhead creeps into the hot path | Flag-off vs flag-on benchmark is a hard acceptance gate (1.3); histogram record path is allocation- and lock-free by design |
| Removing FIFO bias changes published latency numbers | Expected and documented; baseline doc appends a dated re-measurement with explanation, never silently replaces |
| Journal growth from spans | 1-in-N sampling + always-on for rejects; N configurable |
| Windows timer granularity / scheduler noise polluting p99.9 | Report max separately (already doctrine); metadata records power profile; known-noise note in baseline (regime benchmark already shows this pattern) |
| Threshold gate becomes noisy and ignored | Start at 2× baseline tolerance, tighten with variance data; CI job optional/manual first |

## 6. Explicitly out of scope (and where it went)

- Markouts, adverse-selection cost, EV-Gate feedback → Block 2 (consumes this block's clock calibration + trace chain).
- Micro-regime / OFI features → Block 4.
- NTP/PTP deployment and Linux runtime tuning → documented requirements only; execution in capability G's block.
- Any change to signal, gate, risk or OMS decision behavior — this block only observes.
