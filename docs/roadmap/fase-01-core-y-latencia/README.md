# Fase 01 - Core y Latencia

## Objetivo

Make the low-latency core measurable, reproducible and safe to evolve.

## Estado Actual

The event bus, codec, matching benchmark and pipeline benchmark exist. Latency goals are documented as targets but not yet enforced as acceptance gates.

## Problemas Detectados

- Benchmark output and environment metadata need standardization.
- Logging, persistence and queue behavior must be measured under load.
- p50/p95/p99/p99.9 are not uniformly reported across all paths.

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
