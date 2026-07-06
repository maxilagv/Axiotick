# Latency and Scale Targets

This document defines target metrics and the measurement discipline required for Axiotick. Targets are product goals, not current guarantees.

## Target Metrics

| Metric | Target |
| --- | --- |
| Signal analysis latency | p50 < 10 ms / p99 < 50 ms |
| Market data to decision to risk check | p50 < 25 ms / p99 < 100 ms |
| Public API execution latency | 50 ms to 300 ms |
| Colocated HFT latency | Out of scope |
| Market events processed | 100,000 to 1,000,000 events/sec |
| Instruments monitored | 10,000 to 50,000 assets/pairs |
| Exchanges/brokers connected | 8 to 15 |
| Historical data processed | 10 TB to 100 TB |
| Daily data ingested | 100 GB to 2 TB/day |
| Model recalculation | 1s, 5s or 1m by strategy |
| Uptime objective | 99.9% |

## Required Percentiles

Latency reports must include:

- p50
- p95
- p99
- p99.9 when the sample size supports it
- max latency for operational debugging

Average latency is useful only as supporting context.

## Initial Benchmark Suite

- Event bus publish/consume latency and throughput.
- Market tick codec encode/decode.
- Order book matching.
- Risk check latency under realistic order distributions.
- Market data to decision to risk check synthetic pipeline.
- Persistence writer throughput and flush latency.
- API order acknowledgement latency.

## Benchmark Metadata

Every benchmark result must record:

- Hardware model, CPU, memory and storage.
- OS version and power profile.
- Compiler version and build flags.
- Build type and enabled feature flags.
- Dataset size, schema and source.
- Number of instruments and topics.
- Whether logging, persistence and API fanout were enabled.
- Throughput, drops, backpressure hits and queue depth.

## Hot Path Rules

- No blocking disk I/O on market data to risk hot paths.
- No Python execution in live hot paths.
- No unbounded queues in latency-critical runtime paths.
- No hidden sleeps or retry loops in critical path logic.
- Logs must be bounded, async or outside the hot path.
- Allocation-heavy code must be measured before being accepted.

## Current Measurement Gap

The repository includes benchmark targets, but the project still needs a formal benchmark harness, repeatable environment metadata and acceptance thresholds per module. Until then, scale claims must remain target-state documentation.
