# ADR 0006: Serialized OMS + OrderBook Mutation Path

## Context
OrderBook has no internal synchronization and can be accessed by submit/cancel/modify paths.
Without a single lock domain, concurrent operations can interleave and break state consistency.

## Decision
Use OMS mutex as the lock domain for all orderbook mutations:
- `submit_order` now holds OMS mutex through duplicate checks, risk check, matching, and state updates.
- maker updates from fills are applied within the same lock scope.
- helper methods in OMS that mutate state assume caller already holds the mutex.

## Consequences
1. Eliminates race windows between submit/cancel/modify over shared orderbook state.
2. Simplifies correctness reasoning for current single-node architecture.
3. Throughput tradeoff is accepted until per-instrument sharding is introduced.
