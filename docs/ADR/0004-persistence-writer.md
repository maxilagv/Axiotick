# ADR 0004: Asynchronous Persistence and CSV Fallback

## Context
Disk and database I/O must not block the trading path. The system also needs
durable persistence even if TimescaleDB is unavailable.

## Decision
DataWriterService uses a bounded MPSC queue with configurable overflow policy.
It maintains a persistent TimescaleDB connection when enabled and falls back
to CSV with rotation, headers, and optional fsync.

## Consequences
1. Hot path never blocks on I/O.
2. Data remains durable during transient DB outages.
3. Operational parameters (queue size, overflow policy, CSV rotation) are tunable.
