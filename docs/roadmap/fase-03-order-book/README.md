# Fase 03 - Order Book

## Objetivo

Harden the order book as a deterministic, benchmarked component suitable for simulation, paper trading and execution support.

## Estado Actual

The order book supports matching, cancel, partial cancel, modify and benchmark coverage.

## Problemas Detectados

- Concurrency and sharding strategy must be explicit.
- Deterministic replay requirements must be formalized.
- More edge cases are needed for realistic market behavior.

## Subfases

- Freeze order book invariants.
- Define single-thread or shard-per-instrument mutation model.
- Add depth snapshot and replay validation.
- Expand matching edge-case coverage.

## Tareas Tecnicas

- Document price-time priority behavior.
- Test partial fills, queue ordering, cancel/replace and empty book paths.
- Benchmark warm and cold book scenarios.
- Expose book metrics without hot-path blocking.

## Criterios de Aceptacion

- Matching behavior is deterministic.
- Replay produces equivalent book state.
- Benchmarks include p50/p95/p99/p99.9.

## Metricas Esperadas

- Match latency.
- Add/cancel/modify latency.
- Book depth and active orders.
- Replay reconstruction time.

## Tests Requeridos

- Unit tests for all order operations.
- Determinism tests.
- Stress tests for high order counts.

## Benchmarks Requeridos

- Matching benchmark with warm book depth.
- Add/cancel/modify benchmark.

## Riesgos Tecnicos

- Hidden concurrency assumptions can corrupt book state.
- Over-abstracting book structures can hurt latency.

## Dependencias

- Canonical order types and fixed-point behavior.

## Resultado Esperado

A reliable local order book component with measured performance.
