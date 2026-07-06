# Modernization Plan

This plan prioritizes polishing and hardening existing foundations before adding new trading features.

## Principles

- Build, tests and benchmarks must become reproducible before platform expansion.
- Mocks must be clearly marked and isolated from production-intended paths.
- Latency must be measured with percentiles, not described informally.
- Trading decisions must be traceable from input data through signal, risk, order and execution.
- Every order path must pass through risk before execution.
- Documentation must describe actual state and target state separately.

## Phase 1: Repository Truth and Build Hygiene

Tasks:

- Maintain a current audit of implemented, partial and target modules.
- Document supported platforms and known Windows-specific paths.
- Ensure Release builds work from a clean checkout.
- Keep generated build artifacts out of source review.
- Add a repeatable command list for backend build, tests and benchmarks.

Acceptance:

- A new engineer can build and test the project from README instructions.
- No README claim implies production readiness without evidence.

## Phase 2: Test and Benchmark Discipline

Tasks:

- Standardize benchmark output: p50, p95, p99, p99.9, throughput, drops and queue depth.
- Record hardware, OS, compiler and build flags with each benchmark run.
- Add benchmark scenarios for bus, codec, matching, risk, persistence and API order ack.
- Promote critical correctness tests to CI gates.

Acceptance:

- Latency-sensitive changes include benchmark deltas.
- Test failures block release candidates.

## Phase 3: Module Boundary Cleanup

Tasks:

- Identify placeholder modules and mark them as inactive or replace them with real boundaries.
- Separate demo feed playback from production-intended market data gateway interfaces.
- Separate research/backtest/paper/live execution modes in code and config.
- Keep API contracts documented so future clients can be generated or contract-tested.

Acceptance:

- No mock is reachable from a path labeled as live trading.
- API contracts have tests and documented schemas.

## Phase 4: OMS, Risk and Replay Hardening

Tasks:

- Freeze canonical order state transitions.
- Make risk reservations auditable by order id.
- Track reserved, filled and released exposure separately.
- Add deterministic replay checks for order, fill and risk events.
- Add negative tests for malformed orders, duplicate ids and partial fill paths.

Acceptance:

- Replay reconstructs final order/risk state from event journal.
- Risk ledger invariants hold under cancel, replace and partial fill scenarios.

## Phase 5: Observability and Operations

Tasks:

- Add module metrics for latency, queue depth, drops, rejects and errors.
- Define SLOs for local runtime, paper trading and future live trading.
- Add audit events for operator actions and risk decisions.
- Document incident, rollback and kill-switch procedures.

Acceptance:

- Any order can be traced from request to final state.
- Overload behavior is visible and measurable.

## Phase 6: Controlled Expansion

Tasks:

- Add market data connectors only behind normalized gateway interfaces.
- Add signal and strategy engines only after testable data and replay foundations exist.
- Add Python research workflows outside hot execution paths.
- Evaluate Rust only after a component has clear memory-safety or concurrency benefit.

Acceptance:

- New capabilities do not weaken latency measurement, risk checks or auditability.

## 30/90/180/365-Day Priorities

### Next 30 Days

- Freeze current state with audit docs.
- Make build, tests and benchmarks reproducible.
- Separate demo/mock behavior from production-intended paths.
- Define mandatory latency and risk metrics.
- Document module ownership and target state.

### Next 90 Days

- Harden OMS, risk ledger, event journal and replay.
- Build backtesting v1 with costs, spread, slippage and drawdown.
- Implement paper-trading workflow with controlled simulated execution.
- Add p50/p95/p99/p99.9 metrics by critical module.

### Next 180 Days

- Add multi-source market data in paper mode.
- Implement signal engine with EV, risk and traceability.
- Build observability dashboards and alerting.
- Establish security baseline for keys, tokens and roles.

### Next 365 Days

- Reach pilot-grade platform with selected multi-exchange connectivity.
- Add HA design and operational runbooks.
- Add model registry, feature store and governed research workflows.
- Permit controlled live trading only behind readiness checklist approval.
