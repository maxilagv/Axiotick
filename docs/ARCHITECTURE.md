# Architecture

Axiotick is currently a backend-only C/C++ trading engine. The repository is organized around executable entrypoints, domain modules, benchmarks and backend tests.

## Current Repository Layout

```text
backend/
  apps/        runtime and CLI entrypoints
  benchmarks/  benchmark programs
  include/     shared public headers
  modules/     implementation modules by domain
  schema/      schema definitions
  tests/       backend tests
docs/          architecture, roadmap and operating documentation
infra/         optional local infrastructure
scripts/       automation
```

## High-Level Modules

- `modules/datafeed`: low-latency market data parsing, normalization and feed playback.
- `modules/bus`: bounded in-process event bus and message protocol.
- `modules/codec`: market tick encoding/decoding.
- `modules/engine`: order book and matching.
- `modules/risk`: risk checks and exposure accounting.
- `modules/trading`: OMS and order lifecycle.
- `modules/gateway`: routing, venue simulation and execution quality primitives.
- `modules/persist`: event journal and data writer.
- `modules/api`: HTTP/WebSocket gateway.
- `modules/backtest`: historical replay/backtest implementation.
- `modules/core`: shared low-level utilities.
- `modules/network`: socket primitives.

## Current Data Flow

```text
file/replay feed -> datafeed -> codec -> bus -> api / persist / OMS
OMS -> risk -> order book -> event journal
```

## Target Data Flow

```text
market data gateways -> normalizer -> event bus
event bus -> feature/context/signal/strategy/risk/OMS
OMS -> execution gateway -> paper/live venues
all events -> persistence -> replay -> audit -> observability
```

## Message Protocol

- Versioned header with legacy V1 and V2 support.
- Optional CRC flags in V2.
- FlatBuffers support remains optional through `ARGENTUM_USE_FLATBUFFERS`.
- Legacy raw-struct payloads remain supported as compatibility adapters.

## Persistence

- Current: asynchronous writer, event journal and CSV fallback.
- Target: durable event, tick and snapshot storage with deterministic replay and audit retention.

## Observability

- Current: logs, audit messages and initial metrics.
- Target: p50/p95/p99/p99.9 latency, queue depth, drops, rejects, risk decisions, order lifecycle tracing and SLO dashboards.
