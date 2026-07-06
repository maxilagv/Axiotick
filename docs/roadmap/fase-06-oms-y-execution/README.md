# Fase 06 - OMS y Execution

## Objetivo

Turn OMS and execution into a deterministic lifecycle with clear separation between simulated, paper and live paths.

## Estado Actual

OMS supports basic submit, cancel, modify, history and event journal integration. Execution gateway behavior is not live-production ready.

## Problemas Detectados

- Venue acknowledgement, reject and partial-fill handling need formal contracts.
- Live and simulated execution must not share ambiguous paths.
- Order lifecycle APIs must be completed and tested.

## Subfases

- Freeze canonical order state machine.
- Add execution gateway interface.
- Separate simulator, paper and live adapters.
- Add order lifecycle API completeness.

## Tareas Tecnicas

- Define accepted, rejected, resting, partially filled, filled, canceled and replaced transitions.
- Add idempotency handling for duplicate order requests and venue acks.
- Add cancel/replace/list endpoints with contract tests.
- Add execution quality events and reports.

## Criterios de Aceptacion

- Every order transition is deterministic and audited.
- Simulated execution cannot be mistaken for live execution.
- OMS replay reconstructs order history by id.

## Metricas Esperadas

- Order ack latency.
- Cancel/replace latency.
- Rejects by reason.
- Fill ratio and slippage in simulated paths.

## Tests Requeridos

- Order lifecycle state tests.
- API contract tests.
- Duplicate and out-of-order event tests.
- Replay by order id tests.

## Benchmarks Requeridos

- Local OMS submit/cancel/replace benchmark.
- API order acknowledgement benchmark.

## Riesgos Tecnicos

- Ambiguous state transitions can break auditability.
- Venue-specific semantics can leak into OMS core.

## Dependencias

- Risk engine hardening.
- Event journal and replay schema.

## Resultado Esperado

A deterministic OMS/execution boundary ready for paper-trading expansion.
