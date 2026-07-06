# ADR 0009: Smart Order Routing and Execution Simulation v1

## Status
Accepted

## Context
Phase 3 requires multi-venue routing quality and paper-mode execution realism.

## Decision
- Introduce normalized venue abstractions (`VenueQuote`, `VenueDescriptor`, instrument metadata).
- Implement `FixAdapterV1` for FIX market-data normalization.
- Implement `SmartOrderRouter` with fee+latency effective-cost scoring and TIF-aware routing behavior.
- Implement `MarketExecutionSimulator` with queue-ahead, fill probability, slippage, and latency profile.

## Consequences
- Enables venue-aware execution quality tests before live connectivity.
- Creates a deterministic baseline for fill ratio/slippage/latency KPI tracking.
- v1 still assumes top-of-book routing and will later expand to full depth/queue modeling.
