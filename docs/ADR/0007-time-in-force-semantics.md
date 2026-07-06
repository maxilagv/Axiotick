# ADR 0007: Time-In-Force Semantics (GTC, IOC, FOK)

## Context
Phase 1 requires deterministic execution semantics for residual handling and rejection behavior.
The previous OMS behavior always treated residual limit quantity as resting.

## Decision
`Order` now carries `time_in_force` with values:
- `GTC`: residual limit quantity is kept resting.
- `IOC`: execute immediately and cancel any residual.
- `FOK`: reject if full immediate liquidity is not available; otherwise execute fully.

Implementation details:
- OMS pre-checks immediate executable liquidity for `FOK` before risk reservation.
- OrderBook matching accepts a `rest_residual` flag.
- Partial non-resting outcomes are reported as `PartiallyFilled` (not `Filled`).

## Consequences
1. Execution behavior is explicit and predictable per order.
2. API can expose institutional order instructions without hidden behavior.
3. Test matrix now includes GTC/IOC/FOK scenarios and rejection mapping (`liquidity_unavailable`).
