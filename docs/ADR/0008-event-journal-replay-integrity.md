# ADR 0008: Append-Only Event Journal and Deterministic Replay

## Status
Accepted

## Context
Phase 2 requires an auditable event stream (`order_accepted`, `order_rejected`, `trade_executed`, `order_canceled`, `order_replaced`, `gateway_rejected`) with deterministic replay.

## Decision
- Use JSONL append-only journal (`persist::EventJournal`) with monotonic `seq` and `timestamp_ns` checks in replay.
- Emit events from OMS and Gateway paths.
- Reconstruct order lifecycle and risk/position proxies from event stream (`active_orders`, `order_history`, `committed_exposure_units`, `filled_exposure_units`, `net_position_lots`).

## Consequences
- Improves auditability and incident forensics.
- Enables deterministic replay gate in CI.
- JSONL is not the final high-throughput format; can be swapped by preserving schema.
