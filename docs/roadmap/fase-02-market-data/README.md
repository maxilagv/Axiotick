# Fase 02 - Market Data

## Objetivo

Build a production-intended market data gateway and normalization boundary without confusing demo feed playback with real connectivity.

## Estado Actual

File playback, JSON/FIX-like parsing and normalization primitives exist. Real exchange/broker market data connectors are not production-ready.

## Problemas Detectados

- Demo playback is useful but not a market gateway.
- SBE support is not implemented.
- Symbol metadata, reconnect behavior and sequence validation are incomplete.

## Subfases

- Define canonical market event schema.
- Separate replay/file input from live gateway interfaces.
- Add venue metadata and symbol registry.
- Add connector lifecycle states and health metrics.

## Tareas Tecnicas

- Design gateway interface for stream connect, subscribe, resubscribe and disconnect.
- Normalize timestamps, symbols, sides, prices and quantities.
- Track sequence gaps, stale feeds and parse errors.
- Add replay mode using the same canonical schema.

## Criterios de Aceptacion

- Demo feeds are clearly labeled as replay inputs.
- Live gateway interface can support multiple venues.
- Parser errors and sequence gaps are observable.

## Metricas Esperadas

- Parse latency.
- Events/sec per gateway.
- Dropped/invalid messages.
- Feed staleness and reconnect count.

## Tests Requeridos

- Parser tests for each supported format.
- Contract tests for canonical market events.
- Gap and malformed-message tests.

## Benchmarks Requeridos

- Parser throughput.
- Normalization latency.
- Gateway-to-bus publish latency.

## Riesgos Tecnicos

- Venue-specific behavior can leak into core modules.
- Bad timestamp handling can break strategy validation.

## Dependencias

- Event schema discipline.
- Core latency metrics.

## Resultado Esperado

A clean market data boundary ready for real connectors.
