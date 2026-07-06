# ADR 0003: In-Process Bus Backpressure Policy

## Context
The in-process bus must sustain high throughput without unbounded memory growth.
We need predictable behavior when producers exceed consumer capacity.

## Decision
The in-proc bus uses a bounded queue per topic and a configurable backpressure policy:
- DropNewest
- DropOldest
- Block (optional timeout)

Metrics are exposed per topic: queue depth, drops, backpressure hits, publish latency.

## Consequences
1. Memory usage is bounded by design.
2. Producers can detect backpressure via return codes.
3. Metrics support capacity planning and tuning.
