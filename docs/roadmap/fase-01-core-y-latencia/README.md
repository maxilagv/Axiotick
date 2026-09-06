# Fase 01 - Core y Latencia

## Objetivo

Make the low-latency core measurable, reproducible and safe to evolve.

## Estado Actual

The event bus, codec, matching benchmark and pipeline benchmark exist.

**Latency Block 1 (July 2026, ADR 0014/0015) delivered the measurement core of this phase:** one clock module with a CI-enforced discipline gate (`scripts/check_clock_discipline.py`), the shared `core::LatencyHistogram` replacing all sort-based percentile sites, benchmark harness v2 (warmup, JSON output, machine-captured environment metadata) across `matching`/`regime`/`pipeline` plus the previously missing `risk` benchmark, and a regression gate (`scripts/check_benchmarks.py` vs `docs/benchmarks/thresholds.json`). Baseline: [`docs/benchmarks/2026-07-decision-engine-baseline.md`](../../benchmarks/2026-07-decision-engine-baseline.md) (2026-07-08 section).

## Problemas Detectados

- ~~Benchmark output and environment metadata need standardization.~~ Done (harness v2, ADR 0015).
- Logging, persistence and queue behavior must be measured under load (persistence writer benchmark still pending).
- ~~p50/p95/p99/p99.9 are not uniformly reported across all paths.~~ Done (shared histogram, ADR 0015).
- API order-ack benchmark still pending.

## Subfases

- Define benchmark harness and output format.
- Add latency metrics to core modules.
- Establish repeatable local benchmark profiles.
- Gate latency-sensitive changes with benchmark deltas.

## Tareas Tecnicas

- Standardize benchmark result schema.
- Measure bus publish/consume, codec, matching and risk check paths.
- Record queue depth, drops and backpressure hits.
- Document CPU affinity and power-profile assumptions.

## Criterios de Aceptacion

- Benchmarks run from documented commands.
- Results include percentiles and environment metadata.
- Critical hot paths avoid blocking I/O.

## Metricas Esperadas

- p50, p95, p99, p99.9 latency.
- Throughput events/sec.
- Drops and backpressure hits.
- Queue depth under load.

## Tests Requeridos

- Existing bus, codec and order book tests.
- Regression tests for overload behavior.

## Benchmarks Requeridos

- `argentum_matching_benchmark`.
- `argentum_pipeline_benchmark`.
- Future risk and API ack benchmarks.

## Riesgos Tecnicos

- Optimizing before correctness can hide bugs.
- Benchmarks without fixed metadata can mislead decisions.

## Dependencias

- Build reproducibility.
- Stable event schema for measured payloads.

## Resultado Esperado

A measurable latency baseline for all future core changes.
