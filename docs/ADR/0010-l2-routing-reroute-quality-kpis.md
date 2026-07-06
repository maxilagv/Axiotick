# ADR 0010: L2 Routing, Partial-Fill Re-Routing, and Venue Quality KPIs

## Status
Accepted

## Context
Phase 3 v2 requires venue-aware execution over L2 depth, ability to re-route residual quantity after partial fills, and measurable execution-quality KPIs per venue.

## Decision
- Extend normalized venue model to represent L2 depth (`QuoteLevel`, `VenueOrderBookSnapshot`).
- Extend `SmartOrderRouter` with `route_l2` and `reroute_after_partial_fill` while preserving top-of-book compatibility.
- Extend simulator with multi-pass `simulate_with_rerouting` and depth consumption after each simulated fill.
- Introduce `ExecutionQualityTracker` to compute per-venue fill ratio, slippage, and latency percentiles (p50/p95).

## Consequences
- Routing quality and residual handling are now testable in paper mode before real multi-venue rollout.
- Execution quality can be reported with comparable per-venue metrics for tuning router profiles.
- Current implementation remains deterministic and lightweight, but future versions should incorporate full queue-position dynamics and stochastic latency distributions from production telemetry.
