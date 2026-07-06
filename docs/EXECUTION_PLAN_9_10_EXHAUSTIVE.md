# Axiotick Execution Plan

This document has been reset after the repository was converted to a backend-only layout.

Use these current sources of truth:

- `README.md`
- `backend/README.md`
- `docs/CURRENT_STATE_AUDIT.md`
- `docs/ARCHITECTURE.md`
- `docs/ARCHITECTURE_TARGET.md`
- `docs/MODERNIZATION_PLAN.md`
- `docs/roadmap/README.md`
- `docs/LATENCY_AND_SCALE_TARGETS.md`
- `docs/LIVE_TRADING_READINESS_CHECKLIST.md`

## Current Backend Layout

```text
backend/
  apps/        runtime and CLI entrypoints
  benchmarks/  benchmark executables
  include/     public compatibility headers
  modules/     implementation modules by domain
  schema/      wire/schema definitions
  tests/       backend tests
```

## Immediate Execution Priorities

1. Keep the backend-only structure compiling cleanly.
2. Run all backend tests from a clean CMake build.
3. Make benchmark output reproducible and metadata-rich.
4. Harden OMS, risk, persistence and replay.
5. Reintroduce any client UI only after API contracts and backend workflows are stable.
