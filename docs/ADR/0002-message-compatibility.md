# ADR 0002: Message Header Versioning and Compatibility

## Context
We need a header that supports forward/backward compatibility, validation, and
optional integrity checks without breaking existing pipelines.

## Decision
We introduce a versioned header:

- V1: `{version, type, size, timestamp_ns}` (legacy)
- V2: `{version, type, size, timestamp_ns, flags, crc32}`

Decode logic accepts V1 and V2. V2 can optionally carry CRC32 of the payload when
`flags` includes `HasCrc32`.

## Consequences
1. Legacy producers/consumers continue to work via V1.
2. New payloads can enable CRC without changing the bus API.
3. Consumers must validate `size` and `crc32` when present.
