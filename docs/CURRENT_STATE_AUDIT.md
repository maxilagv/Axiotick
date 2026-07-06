# Current State Audit

This audit records the current repository state before adding new trading capabilities. Its purpose is to separate implemented behavior from target architecture and to identify the technical work required to move Axiotick toward a production-grade quantitative trading platform.

## Executive Diagnosis

Axiotick is a strong technical prototype, not yet an institutional trading platform. Current maturity is approximately `4/10` to `4.5/10` against the one-year target.

The repository has credible foundations in C/C++ for low-latency runtime work: bounded in-process messaging, binary protocol support, order book mechanics, OMS flow, risk checks, persistence, replay direction, API contracts and tests. The frontend has been intentionally removed so the project can focus on a production-grade backend core. The main gap is that these foundations are not yet integrated into a production operating model with real market connectivity, complete risk accounting, serious backtesting, paper trading, observability, security and high availability.

## What Is Well Aligned

- C/C++ backend is appropriate for the latency-sensitive core.
- CMake build structure separates backend libraries, executable apps and benchmark targets.
- In-process event bus uses bounded queues and backpressure policies.
- Message protocol and codec work establish a foundation for versioned internal events.
- Order book and OMS provide a base for deterministic order lifecycle work.
- Risk manager has initial order validation, exposure and symbol-limit checks.
- Event journal and replay-oriented tests point in the right auditability direction.
- API gateway already exposes health, order and market interfaces for external clients and future operator tools.
- Benchmarks exist for matching and pipeline paths, although methodology must be hardened.
- Frontend has been removed to keep the repository backend-first and avoid maintaining prototype UI code.

## Prototype, Demo or Insufficient Areas

- Exchange gateway implementations are not real production connectors.
- Market data ingestion is file/demo-oriented and not multi-venue.
- Backtesting is too limited for institutional-quality strategy validation.
- Python/AI integration is not a production model pipeline.
- Strategy and signal logic are mostly interfaces and primitives, not a robust engine.
- No frontend/operator console exists now; any future UI should be introduced as a separate product decision.
- High availability, failover and uptime operations are not implemented.
- Security controls are preliminary and not enough for live trading.
- Compliance, surveillance and governance are not yet platform capabilities.
- Scale targets are not proven by reproducible benchmark evidence.

## Strongest Modules

- **Event bus:** good foundation for bounded internal messaging and latency metrics.
- **Order book:** useful base for matching, cancel and modify behavior.
- **OMS:** credible first order lifecycle layer with journal integration.
- **Risk manager:** useful pre-trade base, but needs deeper accounting.
- **Event journal:** important for audit and replay direction.
- **Tests:** current test coverage is meaningful for a prototype and should become a gate.

## Modules That Need Major Rework

- **Market connectivity:** replace demo/logging adapters with real connector architecture.
- **Backtesting:** rebuild around historical data, costs, slippage, spread, funding and bias controls.
- **Signal engine:** move from simple strategy primitives to EV/risk/context-aware signals.
- **AI/research:** define Python boundary, feature store, model registry and validation lifecycle.
- **Security:** design secrets, RBAC, token lifecycle, TLS, audit and operational controls.
- **Observability:** add metrics, traces, dashboards, SLOs and benchmark reporting discipline.

## Technical Risks

- Mocks and production-like names can create false confidence.
- Order/risk accounting must remain deterministic under partial fills, cancels and replaces.
- Latency claims can become meaningless without fixed benchmark methodology.
- Windows-specific socket paths limit deployability.
- Mixing research, paper and live code paths can create dangerous operational ambiguity.
- Event schemas and API contracts can drift without contract tests.
- Persistence and replay must be hardened before relying on audit reconstruction.

## Technical Debt

- Legacy placeholder module folders were removed during backend restructuring.
- README and docs previously mixed demo status with ambitious target claims.
- Some benchmarks are not yet repeatable as formal acceptance gates.
- API parsing and contract handling need stronger validation boundaries.
- Future UI/client schemas will need generated or contract-tested alignment with backend APIs.
- Hardcoded development defaults must be separated from production configuration.

## Current Architecture

Current architecture is a backend-only C/C++ system with modular static libraries:

```text
backend/apps -> backend/modules -> backend/include compatibility layer
file/demo feed -> parser/codec -> in-process bus -> writer/api/OMS/order book/risk
```

The system is useful for local development and component validation. It is not yet a distributed, high-availability or multi-venue architecture.

## Target Architecture

Target architecture should become event-oriented and mode-separated:

```text
market data gateways -> normalizer -> event bus
event bus -> feature/context/signal/strategy/risk/OMS
OMS -> execution gateway -> paper/live venues
all events -> persistence -> replay -> audit -> observability
research -> feature store/model registry -> controlled runtime promotion
```

The target does not require immediate microservices. The first priority is deterministic module boundaries, measurable latency and clear separation of live, paper, backtest and research paths.

## Latency Impact Areas

- Parser and codec allocation behavior.
- Event bus queue capacity, backpressure and consumer scheduling.
- Order book data structures and mutation path.
- Risk check complexity and lock contention.
- Logging and persistence on hot paths.
- API and WebSocket fanout paths.
- Python integration boundaries, which must not sit directly in hot execution paths.

## Reliability Impact Areas

- Replay determinism and event schema stability.
- Backpressure behavior under overload.
- Drop accounting and alerting.
- Idempotent order lifecycle transitions.
- Persistence durability and recovery.
- Health checks, SLOs and operational runbooks.

## Security, Risk and Auditability Impact Areas

- API token lifecycle and secrets management.
- Live trading kill switch and permission separation.
- Immutable audit log with order, risk and operator events.
- Risk ledger correctness.
- Model and strategy versioning.
- Replay from raw events to final state.
- Clear prohibition of live execution without readiness gates.

## Distance to Final Objective

The repository is closer to a serious prototype than a product. The foundations are worth preserving, but the project still needs substantial engineering across risk, market data, strategy validation, operations, security and documentation before it can support live trading or investor-grade diligence.
