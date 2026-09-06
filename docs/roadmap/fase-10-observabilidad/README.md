# Fase 10 - Observabilidad

## Objetivo

Make system behavior visible: latency, throughput, drops, risk decisions, orders, errors and operator actions.

## Estado Actual

Basic logging, audit messages and some API metrics exist. Full observability is not implemented.

**Latency Block 1 (July 2026, ADR 0014) delivered the trace backbone of this phase:** `TraceSpans` with per-stage monotonic stamps through tick → signal → gate → risk → OMS → journal, `tick_id` minted at ingress and journaled with every order event (cancels/replaces included — that correlation bug is fixed and regression-tested), per-decision spans in the `signal_decision` audit event (all rejects + 1-in-N accepts), and the wire-to-decision percentile report in the demos and Mode B backtests.

## Problemas Detectados

- Metrics are not consistent across modules.
- No full SLO dashboard.
- No incident runbooks or alert routing.
- ~~Tracing from signal to execution is incomplete.~~ In-process chain done (ADR 0014); venue-side ack tracing lands with paper trading.

## Subfases

- Define metric taxonomy.
- Add module-level latency and health metrics.
- Add decision and order trace ids.
- Build dashboards and alert rules.

## Tareas Tecnicas

- Emit metrics for bus, parser, signal, risk, OMS, execution and persistence.
- Track drops, stale data, rejects and queue depth.
- Add structured audit events.
- Document operational runbooks.

## Criterios de Aceptacion

- Any order can be traced through the platform.
- Critical latency percentiles are visible.
- Alerts fire for stale data, disconnects and risk breaches.

## Metricas Esperadas

- p50/p95/p99/p99.9 latency by component.
- Events/sec.
- Queue depth and drops.
- Error and reject rates.
- Uptime and service health.

## Tests Requeridos

- Metrics emission tests.
- Alert threshold tests where practical.
- Trace id propagation tests.

## Benchmarks Requeridos

- Observability overhead benchmark.
- Logging/audit throughput benchmark.

## Riesgos Tecnicos

- Metrics can add hot-path overhead.
- Unstructured logs make incident response slow.

## Dependencias

- Stable component boundaries.
- Event schema discipline.

## Resultado Esperado

An operator can understand system health and reconstruct critical workflows.
