# Plan

The current strategic plan is maintained in:

- `docs/roadmap/README.md`
- `docs/MODERNIZATION_PLAN.md`
- `docs/ARCHITECTURE_TARGET.md`
- `docs/CURRENT_STATE_AUDIT.md`

## Immediate Direction

1. Keep Axiotick backend-only.
2. Stabilize the reorganized backend layout.
3. Make build, tests and benchmarks reproducible.
4. Harden OMS, risk, persistence and replay before adding new features.
5. Add market data, signal, paper trading and live execution capabilities only behind explicit readiness gates.

## Current Repository Structure

- `backend/apps`: runtime and CLI entrypoints.
- `backend/modules`: implementation modules by domain.
- `backend/include`: public compatibility headers.
- `backend/benchmarks`: latency and throughput benchmarks.
- `backend/tests`: backend test targets.
- `docs`: architecture, roadmap and operational documentation.
