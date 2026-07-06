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
