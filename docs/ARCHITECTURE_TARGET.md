# Target Architecture

Axiotick should evolve into an event-oriented trading platform with strict separation between research, backtesting, paper trading and live trading. The architecture can remain modular in-process where latency requires it, but component boundaries must be explicit and testable.

## Component Matrix

| Component | Responsibility | Inputs | Outputs | Latency Requirement | Current State | Target State |
| --- | --- | --- | --- | --- | --- | --- |
| Market Data Gateway | Connect to exchanges, brokers and data providers | Venue streams, REST snapshots, files for replay | Raw venue events | Low jitter, high throughput | File/demo-oriented plus placeholder gateway | Multi-source, reconnecting, rate-limit aware gateways |
| Normalizer | Convert venue messages to canonical events | Raw venue events | Canonical market events | Sub-ms to low ms per event | Basic parser/normalizer exists | Versioned schema with validation and symbol metadata |
| Event Bus | Move events through runtime | Canonical events, orders, risk events | Topic streams | p99 measured under load | In-process bounded bus exists | Measured internal bus with clear overload semantics |
| Order Book Engine | Maintain book state and matching/simulation | Normalized book/order events | Book state, trades, snapshots | Microsecond to low ms local paths | Matching/cancel/modify base exists | Deterministic, sharded, benchmarked book engine |
| Signal Engine | Compute probabilistic signals | Market features, context, models | Signal events, EV inputs | p50 < 10 ms / p99 < 50 ms | Minimal strategy interface | EV/context-aware signal pipeline |
| Historical Context Engine | Compare current regimes with history | Feature vectors, historical datasets | Similarity, distributions, regime labels | Strategy-dependent | Not implemented | Searchable context store and regime analytics |
| Strategy Engine | Select actions and sizing | Signals, context, risk budget | Candidate orders | Low ms to strategy window | Minimal interface | Versioned strategies with explainable decisions |
| Risk Engine | Enforce pre-trade/runtime controls | Candidate orders, positions, limits | Accept/reject, risk events | Included in p99 < 100 ms path | Basic exposure checks | PnL, VaR/CVaR, drawdown, correlation and kill switch |
| OMS | Own order lifecycle | Risk-approved orders, venue acks/fills | Order state, events | Low ms local state transitions | Base lifecycle exists | Deterministic event-sourced lifecycle |
| Execution Gateway | Route to venues or simulators | OMS orders | Venue orders, acks, fills | Public API venue-bound 50-300 ms | Simulated/logging behavior | Paper/live adapters behind common interface |
| Backtesting Engine | Validate strategies historically | Historical data, strategy versions | Metrics, trades, reports | Offline throughput focused | Basic replay-oriented loader | Bias-aware engine with realistic costs |
| Paper Trading Engine | Run real-time simulation | Live data, strategies, simulated venues | Paper fills and metrics | Close to live runtime | Not complete | Venue latency/slippage simulator and reports |
| Live Trading Engine | Controlled real capital execution | Approved orders and limits | Live venue orders | Venue/network dependent | Not ready | Enabled only after readiness checklist |
| Feature Store | Store reusable features | Raw/canonical data | Online/offline features | Online path measured | Not implemented | Versioned feature definitions and access |
| Model Registry | Track models and validation | Trained models, metrics | Approved model versions | Not hot path | Not implemented | Promotion, rollback and evaluation registry |
| AI/Research Layer | Research, training, NLP, notebooks | Historical data, news, features | Models, reports, hypotheses | Outside live hot path | Placeholder bridge | Python research environment with controlled promotion |
| Observability Layer | Metrics, logs, traces, alerts | Runtime telemetry | Dashboards, alerts | Must not block hot path | Basic logs/metrics | SLO dashboards and incident runbooks |
| Persistence Layer | Store ticks, events, snapshots | Runtime events | Durable records | Async where possible | CSV/optional DB path | Tiered storage for hot/warm/cold data |
| Replay Engine | Reconstruct system state | Event journals, snapshots | Deterministic state | Offline correctness | Initial journal/replay tests | Full order/risk/strategy replay |
| Audit Log | Record decisions and operator actions | Orders, risk, config, user actions | Immutable audit stream | Async non-blocking | Initial audit logs | Tamper-evident event trail |
| Security Layer | Protect access and secrets | Users, tokens, keys, config | Auth decisions | Low overhead | Token/rate limit base | RBAC, rotation, TLS, secrets management |
| Configuration System | Manage runtime settings | Environment, files, profiles | Validated config | Startup and controlled reload | Env/dev defaults | Typed, validated, environment-specific config |
| Benchmark Suite | Measure latency and throughput | Synthetic and replay datasets | Percentile reports | Reproducible | Benchmark targets exist | CI/manual benchmark harness with metadata |
| Testing Framework | Guard correctness | Unit, integration, contracts, replay | Pass/fail evidence | CI feedback | Useful tests exist | Layered gates for correctness and regressions |

## Data Flow

```text
external venues/providers
  -> market data gateway
  -> normalizer
  -> event bus
  -> order book / feature store / historical context
  -> signal engine
  -> strategy engine
  -> risk engine
  -> OMS
  -> execution gateway
  -> paper simulator or live venue
```

All important events also flow to persistence, replay, audit and observability.

## Language Boundaries

- **C/C++:** latency-sensitive runtime, order book, OMS, risk, codecs, bus and execution paths.
- **Python:** research, model training, NLP, notebooks, historical analytics and offline validation.
- **Rust:** optional future addition for connectors, parsers or concurrent services where memory safety and maintainability provide a concrete advantage. Rust should not be added until a specific component justifies the extra build and operational complexity.

## Architecture Risks

- Over-splitting services before correctness is proven can increase latency and operational burden.
- Allowing Python into hot live paths can break latency guarantees.
- Treating simulated connectors as live connectors can create dangerous false readiness.
- Adding AI without model governance can weaken auditability.
- Ignoring benchmark metadata can make latency claims impossible to compare.
