# ADR 0001: Message Schema and Payload Encoding

## Context
Axiotick needs a stable, versioned payload contract for MarketTick, Order, and Trade.
The current implementation uses raw `memcpy` of C structs, which is brittle across
language boundaries and compiler/ABI differences.

## Decision
We adopt FlatBuffers for payload encoding. The initial schema is defined in
`backend/schema/argentum.fbs` and includes `MarketTick`, `Order`, and `Trade`.
The protocol header remains outside the FlatBuffers payload and carries versioning,
type, size, timestamp, and optional CRC.

## Consequences
1. Payloads become language-agnostic and forward-compatible.
2. Legacy binary payloads remain supported via a temporary adapter.
3. Build toolchain gains an optional `flatc` dependency when FlatBuffers is enabled.
