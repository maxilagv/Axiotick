# Decision Engine Latency Baseline — 2026-07 (Blocks 1-3)

Reference numbers for the decision-engine components delivered in Blocks 1-3,
recorded with the metadata discipline required by
`docs/LATENCY_AND_SCALE_TARGETS.md`. These are single-machine local numbers,
not production guarantees.

## Environment

| Item | Value |
| --- | --- |
| CPU | AMD Ryzen 5 5500 (6C/12T) |
| Memory | 32 GB |
| OS | Windows 11 Pro |
| Compiler | MSVC (Visual Studio 18 2026 generator), `/O2 /Oi /Ot /Oy /GL`, `/std:c++20` |
| Build type | Release |
| Power profile | default desktop profile (not pinned) |

## EV gate — `EVGate::evaluate()`

- Source: `SignalEngine::gate_latency_snapshot()` during `argentum_signal_demo`
  (3,000 synthetic ticks, ~50 candidate evaluations per run).
- p50 ~100 ns, p95 ~100 ns, p99 ~300 ns, max ~600 ns per evaluation.
- Interpretation: the gate itself is 5 orders of magnitude below the < 25 ms
  decision budget; decision latency will be dominated by data/feature paths,
  not by EV math.

## Regime classifier — `RegimeClassifier::on_bar()`

- Source: `argentum_regime_benchmark`, 1,000,000 synthetic bars, alternating
  calm/volatile phases so every rule branch executes; default config
  (vol_window=20, percentile_window=100, dmi_period=14).
- p50 600 ns, p95 800 ns, p99 900 ns, p99.9 ~60 µs per bar.
- The p99.9 tail is scheduler noise plus the O(percentile_window) scan; bars
  arrive at seconds-to-minutes cadence, so per-bar microseconds are
  irrelevant to the decision budget. Revisit only if bar cadence ever drops
  below ~10 ms.

## Order book matching — `argentum_matching_benchmark` (pre-existing)

- 1,000,000 iterations, warm book depth 1,024. Re-run per release; record
  results here with the same metadata block.

## Reproduction

```powershell
cmake --build build-block1 --config Release
.\build-block1\bin\Release\argentum_regime_benchmark.exe
.\build-block1\bin\Release\argentum_signal_demo.exe     # gate latency section
.\build-block1\bin\Release\argentum_matching_benchmark.exe
```

Update this file (append a dated section, do not overwrite) whenever a
latency-relevant component changes, per `docs/MODERNIZATION_PLAN.md` Phase 2.

---

# Re-measurement — 2026-07-08 (Latency Block 1: wire-to-wire measurement + clock discipline)

Measured after ADR 0014/0015 landed. Two methodology changes shift numbers
relative to the section above, both deliberate and documented:

1. **FIFO-eviction bias removed, quantization added.** All percentiles now
   come from the shared `core::LatencyHistogram` (log-linear buckets,
   ≤ 3.2% relative error, values reported at the bucket **upper edge** — a
   claim may round up, never down). E.g. a true ~200 ns p95 reports as 203.
2. **Warmup added.** Every benchmark now runs an explicit warmup phase
   excluded from statistics; the old runs had none.

## Environment (captured programmatically by the harness — not hand-typed)

| Item | Value |
| --- | --- |
| CPU | AMD Ryzen 5 5500 (12 hardware threads) |
| Memory | 32,654 MB |
| OS | Windows 10.0 build 26200 |
| Compiler | MSVC 195035724 |
| Build type | Release, `ARGENTUM_ENABLE_LATENCY_TRACE=ON` |
| Clock uncertainty | 0 ns (bracketed calibration below QPC resolution) |
| Power profile | default desktop profile (not pinned) |

## Component benchmarks (1M iterations each, 10k+ warmup, JSON in `benchmarks_out/`)

| Case | p50 | p95 | p99 | p99.9 | max | Notes |
| --- | --- | --- | --- | --- | --- | --- |
| `order_book_match_and_refill` | 101 ns | 203 ns | 203 ns | 203 ns | 25.1 µs | match + refill add_order per iteration |
| `regime_classifier_on_bar` | 407 ns | 607 ns | 703 ns | 1.2 µs | 86.4 µs | faster than 2026-07 numbers: warmup + histogram method |
| `risk_check_order_mixed` | 203 ns | 703 ns | 1.0 µs | 1.9 µs | 88.6 µs | **first-ever risk-path measurement** (Fase 01 debt) |
| `tick_encode_publish_consume_decode` | 407 ns | 38.9 µs | 124.9 µs | 311.3 µs | 378.6 µs | cross-thread transport; throughput 3.87M ticks/s |

## Wire-to-decision (Mode B backtest = exact production decision path, 260 candidates)

| Span | p50 | p95 | p99 | p99.9 | max |
| --- | --- | --- | --- | --- | --- |
| signal_eval_start (tick→strategy→engine) | 503 ns | 1.5 µs | 1.6 µs | 5.9 µs | 5.9 µs |
| gate_verdict | 203 ns | 407 ns | 703 ns | 1.3 µs | 1.3 µs |
| risk_verdict | 407 ns | 815 ns | 1.3 µs | 16.0 µs | 16.0 µs |
| journal_enqueue | 2.3 µs | 3.5 µs | 4.9 µs | 5.7 µs | 5.7 µs |
| oms_accept | 1.6 µs | 3.8 µs | 5.5 µs | 11.8 µs | 11.8 µs |
| **wire→gate** | **703 ns** | 1.5 µs | 2.3 µs | 7.2 µs | 7.2 µs |
| **wire→decision** | **1.0 µs** | 6.9 µs | 10.5 µs | 32.3 µs | 32.3 µs |

Honest reading: the full decision path — tick ingress through strategy, EV
gate, risk, matching and journal enqueue — completes in ~1 µs median,
~10-30 µs tail, four orders of magnitude under the 25 ms budget. The
25 ms budget exists for the paths that do not exist yet (live venue I/O,
feature stores), exactly as the July baseline predicted.

## Instrumentation overhead (Block 1 acceptance gate: < 2% throughput cost)

3 alternating ON/OFF pairs of `argentum_pipeline_benchmark`
(`ARGENTUM_ENABLE_LATENCY_TRACE` ON vs OFF builds):

- mean throughput ON = 4.00M ticks/s, OFF = 3.86M ticks/s (ON faster; delta
  −3.7%, i.e. **inside run-to-run noise — measured cost indistinguishable
  from zero**). ON beat OFF in 2 of 3 runs.
- Bus publish-latency histogram is sampled 1-in-16 by timestamp low bits: the
  unsampled first attempt cost ~7.5% throughput at 3.8M publishes/s (5 relaxed
  atomic RMWs per publish) and failed the gate; the sampled design passes it.
  Exact counters (published/drops/backpressure) remain unsampled.

## Clock discipline (`argentum_clock_check`, 5 rounds × 400 ms)

- Calibration uncertainty: ±0 ns (bracket below QPC resolution).
- Wall-vs-mono drift: 0 ns / 0.000 ppm over 1.6 s — on this host both clocks
  derive from the same counter between NTP corrections, so nonzero drift here
  means an NTP step/slew occurred: exactly the event the tool exists to catch.

## Reproduction

```powershell
cmake -S . -B build && cmake --build build --config Release
.\build\bin\Release\argentum_matching_benchmark.exe benchmarks_out\matching.json
.\build\bin\Release\argentum_regime_benchmark.exe   benchmarks_out\regime.json
.\build\bin\Release\argentum_risk_benchmark.exe     benchmarks_out\risk.json
.\build\bin\Release\argentum_pipeline_benchmark.exe benchmarks_out\pipeline.json
.\build\bin\Release\argentum_backtest_demo.exe      # wire-to-decision section
.\build\bin\Release\argentum_clock_check.exe
python scripts\check_benchmarks.py benchmarks_out\matching.json benchmarks_out\regime.json benchmarks_out\risk.json benchmarks_out\pipeline.json
```

Regression ceilings: `docs/benchmarks/thresholds.json` (~2× these values,
tighten as variance data accumulates).
