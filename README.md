# Axiotick

Axiotick is a low-latency quantitative trading platform in active development. The project is intended to evolve into a serious research, backtesting, paper-trading and controlled live-trading system for professional traders, small funds, quantitative teams and financial technology builders.

The platform is not positioned as a finished trading product. It is a C/C++ trading engine with early infrastructure for market data, an in-process event bus, order book mechanics, OMS flow, risk checks, persistence, replay and API access. The next stage is to harden the existing backend core before adding broader market connectivity, production AI workflows or live execution.

## Product Vision

Axiotick aims to provide an auditable quantitative trading platform where every signal, order, risk decision and execution event can be measured, replayed and explained. The long-term objective is not to build a generic trading bot, but a disciplined platform for probabilistic signals, positive expected value, risk-adjusted execution and controlled automation.

The platform separates four modes:

- **Research:** Python-driven analysis, feature engineering, model training, NLP and historical studies.
- **Backtesting:** deterministic simulation with costs, spread, slippage, funding, drawdown and out-of-sample validation.
- **Paper trading:** real-time strategy execution against simulated venues, latency profiles and risk controls.
- **Live trading:** controlled production execution only after validation, monitoring, kill switches and operational approval.

## Problem

Most retail and early-stage trading systems fail because they mix research, execution, risk and operations into one opaque workflow. They often lack reproducible latency measurements, realistic costs, audit trails, replay, risk governance and clear separation between simulated and live capital.

Axiotick is designed to address those gaps by making latency, risk, traceability and validation first-class engineering concerns.

## Current State

The decision-engine pillar is substantially implemented and tested; the
connectivity, AI-pipeline and operations pillars remain open. Overall
maturity against the one-year target architecture is approximately `5/10`
to `5.5/10` (decision engine well ahead of that line, operations behind it).

Implemented and tested (evidence: 38 CTest targets, two end-to-end demo
binaries, ADRs 0011-0013):

- C/C++ backend with CMake build targets.
- In-process event bus with bounded queues and backpressure policies.
- Binary message protocol and market tick codec.
- Order book with matching, cancel and modify paths.
- OMS lifecycle base with event journal integration; orders journaled with
  the originating signal id for replay correlation.
- EV-gated signal engine: net-expected-value gate over modeled costs (fees,
  spread, slippage, market impact, funding x holding time), fail-closed on
  malformed inputs, full explainability audit trail per decision.
- Regime detection: tick->bar aggregation and a deterministic classifier
  (trend / range / stress / vol-expansion) conditioning gate thresholds and
  strategy eligibility; latency benchmarked.
- Strategy lifecycle governance: automatic active / under-observation /
  quarantined / disabled transitions driven by Page-Hinkley break detection,
  rolling-Sharpe floor, EV-decay t-stat and strategy drawdown, with audited
  transitions and manual operator override.
- Risk engine: order value, exposure and symbol limits; average-cost position
  accounting with realized/unrealized PnL; daily loss limit with automatic
  kill switch (manual reset only).
- Backtesting: counterfactual runs through the exact production decision
  path, Sharpe/Sortino/max-drawdown/historical VaR-CVaR, deterministic Monte
  Carlo bootstrap, walk-forward out-of-sample validation, per-regime
  realized-vs-predicted reporting and cost-sensitivity/stress sweeps.
- HTTP/WebSocket API gateway with token and rate-limit controls.
- Asynchronous market tick writer with CSV fallback and optional database path.
- Event journal and replay-oriented tests.
- Matching, pipeline and regime benchmark targets with a recorded baseline
  (`docs/benchmarks/`).
- Backend-only repository structure with runtime apps, modules, benchmarks and tests separated.

Still in development:

- Production-grade multi-exchange/broker connectivity and live market data
  (funding, open interest, liquidations feeds).
- Production AI/ML pipeline: real Python model serving (the current bridge is
  a stub), feature store and model registry.
- Paper-trading mode against live data; execution-venue adapters.
- Portfolio-level margin/correlation controls and runtime VaR gating.
- Java service layer (reporting/audit query, risk snapshots, model registry).
- High availability, distributed deployment and 99.9% uptime operations.
- Security, compliance, surveillance and enterprise governance.
- Reproducible scale evidence for 100k to 1M events/sec and 10k to 50k instruments.

## Architecture Overview

Repository layout:

```text
backend/
  apps/           executable entrypoints
  benchmarks/     latency and throughput benchmark programs
  include/        shared public headers
  modules/        backend implementation modules by domain
  schema/         wire/schema definitions
  tests/          backend test targets
docs/             architecture, roadmap, audit and readiness documentation
infra/            optional local infrastructure
scripts/          repository automation
```

Target data flow:

```text
Market Data Gateways
  -> Normalizer
  -> Internal Event Bus
  -> Order Book / Feature Store / Historical Context
  -> Signal Engine
  -> Strategy Engine
  -> Risk Engine
  -> OMS
  -> Execution Gateway
  -> Venue / Broker

All events
  -> Persistence
  -> Replay
  -> Audit Log
  -> Observability
```

Core components:

- **Market Data Gateway:** receives external market data from exchanges, brokers and data providers.
- **Normalizer:** converts venue-specific messages into canonical internal events.
- **Event Bus:** moves bounded, measurable event streams through the runtime.
- **Order Book Engine:** maintains local book state and matching/simulation primitives.
- **Signal Engine:** computes probabilistic signals and expected-value inputs.
- **Strategy Engine:** evaluates strategy rules, context and sizing.
- **Risk Engine:** enforces pre-trade and runtime risk controls.
- **OMS:** owns order lifecycle, state transitions and audit events.
- **Execution Gateway:** routes validated orders to simulated or live venues.
- **Backtesting/Paper Trading:** validates strategy behavior before live capital.
- **AI/Research Layer:** uses Python for research, training and explainability workflows.
- **Observability and Audit:** captures latency, drops, decisions, events and operator actions.

See [docs/ARCHITECTURE_TARGET.md](docs/ARCHITECTURE_TARGET.md) for the target architecture.
See [docs/LANGUAGE_STRATEGY.md](docs/LANGUAGE_STRATEGY.md) for the Python and Rust strategy.

## Latency and Scale Targets

These are product targets, not current production guarantees:

- Signal analysis latency: p50 `< 10 ms`, p99 `< 50 ms`.
- Market data to decision to risk check: p50 `< 25 ms`, p99 `< 100 ms`.
- Public API execution latency: `50 ms` to `300 ms`, depending on venue and network.
- HFT colocated execution: out of scope for a normal independent operator.
- Market events processed: `100,000` to `1,000,000` events/sec.
- Instruments monitored: `10,000` to `50,000` assets/pairs.
- Exchanges/brokers connected: `8` to `15`.
- Historical data processed: `10 TB` to `100 TB`.
- Daily data ingested: `100 GB` to `2 TB/day`.
- Model recalculation: every `1s`, `5s` or `1m`, depending on strategy.
- Uptime objective: `99.9%`.

Latency reporting must use p50, p95, p99 and p99.9 where relevant. Benchmarks must include hardware, compiler, build profile, dataset size, throughput, drops and queue depth.

See [docs/LATENCY_AND_SCALE_TARGETS.md](docs/LATENCY_AND_SCALE_TARGETS.md).

## Roadmap

The project evolves through long, verifiable phases:

1. Audit and repository truth.
2. Core latency and build reproducibility.
3. Market data ingestion and normalization.
4. Order book correctness and performance.
5. Signal and strategy engines.
6. Risk engine hardening.
7. OMS and execution workflows.
8. Institutional backtesting.
9. Paper trading.
10. AI and research platform.
11. Observability.
12. Security and governance.
13. Multi-exchange connectivity.
14. High availability and production operations.
15. Finance-ready product packaging.

See [docs/roadmap/README.md](docs/roadmap/README.md).

## Build

Use a Windows shell with a supported C/C++ compiler available to CMake. On this machine the project configures with Visual Studio 18 2026; Visual Studio 2022 is also supported where installed.

```powershell
cmake -S . -B build
cmake --build build --config Release
```

Run the backend node:

```powershell
.\build\bin\Release\argentum_node.exe
```

Current internal binary and library names still use the legacy `argentum_*` prefix. The product name is Axiotick; technical identifiers will be renamed in a separate compatibility-aware phase.

Optional infrastructure:

```powershell
docker compose -f infra/docker-compose.yml up -d
```

## Tests

Run CTest after building:

```powershell
ctest --test-dir build -C Release --output-on-failure
```

If using another build directory, replace `build` with the selected directory.

## Benchmarks

Build Release first, then run:

```powershell
.\build\bin\Release\argentum_matching_benchmark.exe
.\build\bin\Release\argentum_pipeline_benchmark.exe
```

Benchmark results are only useful when recorded with:

- CPU, memory, OS and power profile.
- Compiler and build flags.
- Input dataset size and event schema.
- p50, p95, p99 and p99.9 latency.
- Throughput, drops, backpressure hits and queue depth.
- Whether persistence, logging and API paths were enabled.

## Technical Principles

- Latency is a product metric, not an afterthought.
- Trading decisions must be explainable and replayable.
- Every order must pass risk checks before execution.
- Research, backtesting, paper trading and live trading must remain separate.
- Expected value, costs, slippage, spread, funding and drawdown matter more than raw win rate.
- Mocks and simulations must be clearly marked and isolated from production paths.
- Claims in documentation must match implemented behavior or be labeled as target state.
- Security, auditability and operational controls are required before live trading.

## Contributing

Contributions should improve correctness, latency measurement, testability, auditability or production readiness. Avoid decorative features, unverified performance claims, hidden mocks and abstractions that obscure trading or risk behavior.

Before submitting changes:

- Build in Release mode.
- Run relevant tests.
- Add or update documentation when behavior changes.
- Include benchmark evidence for latency-sensitive changes.
- Explain risk impact and rollback considerations.

## Financial and Risk Notice

Axiotick is not financial advice. The repository is not a recommendation to buy, sell or trade any asset.

Live trading requires independent validation, risk limits, secure key management, operational monitoring, kill switches, audit trails and human supervision. No AI or quantitative model in this project should be treated as a certainty engine. Signals must be interpreted probabilistically and evaluated through expected value, costs, risk and validation discipline.
