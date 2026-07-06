# Axiotick Roadmap: from 4.5/10 to 9/10

## 1) Current state summary (evidence-based)

Current project status is a strong technical prototype, not yet an institutional trading platform.

What is already real:
- In-proc low-latency pipeline with bounded queues and backpressure.
- Basic OMS + order book with matching and cancel/modify.
- Basic risk checks (order value and exposure limits).
- HTTP/WS gateway with token auth, rate limiting, and `/metrics`.
- Backend-only layout with apps, modules, benchmarks and tests separated.

What is still demo/mock:
- Backtest uses synthetic data and returns fake metrics.
- Python bridge returns a dummy prediction.
- Single-venue connectivity and no FIX/SBE adapters.
- No formal compliance stack (surveillance, reporting, KYC/AML).

## 2) Maturity score by pillar

1. Market connectivity and latency: `3/10`
- Good internal core patterns, but no multi-venue DMA/SOR and no kernel-bypass stack.

2. Order engine and position lifecycle: `6/10`
- Matching, cancel, partial cancel, and modify exist.
- Missing advanced order types (IOC/FOK/GTC, trailing, iceberg), portfolio-level position services, and institutional controls.

3. Analysis and backtesting: `3/10`
- Backtester exists but currently mock-oriented (synthetic + fake output).

4. Risk and compliance: `4/10`
- Initial risk checks exist.
- Missing full intraday margining, VaR in runtime controls, surveillance, and regulator-grade audit/reporting.

5. API and client surface: `4/10`
- Streaming and order submit endpoints exist.
- Frontend has been removed; future client/operator surfaces should be rebuilt only after backend contracts stabilize.
- Missing full order lifecycle API set, portfolio/risk endpoints, and institutional client workflows.

6. AI and data science: `2/10`
- Interface exists, production ML pipeline does not.

7. Observability, testing, DevOps: `5/10`
- Test suite, CI skeleton, and metrics endpoint are present.
- Missing distributed tracing, SLO-based operations, chaos/load/fuzz maturity at scale, and HA deployment discipline.

8. GTM and monetization readiness: `2/10`
- No formal packaging/pricing/commercial controls yet.

Weighted total: about `4.5/10` (aligned with your current view).

## 3) Roadmap to 9/10 (phased and finance-ready)

## Phase A (0-12 weeks): Institutional MVP
Goal: convert prototype into a credible pilot platform for selective B2B customers.

Technical deliverables:
1. Order lifecycle completion
- Add `IOC`, `FOK`, `GTC`, mass cancel, replace-by-id, and deterministic state transitions.
2. Event-sourced persistence + replay
- Persist all order/trade events to append-only journal and deterministic replay tooling.
3. Runtime risk hardening
- Add real-time PnL, net exposure by instrument/account, max loss guardrails, kill switch.
4. API v1 expansion
- Endpoints for create/cancel/replace/list orders, positions, account risk, and execution history.
5. Client/operator API readiness
- Complete backend contracts for future risk panels, order state timelines and latency diagnostics.
6. Test strategy uplift
- Add contract tests for all API paths and deterministic replay tests in CI.

Exit KPIs:
- Deterministic replay reproducibility `>= 99.99%`.
- API p99 latency for order ack in local bench with target load.
- Zero critical findings in order-state consistency tests.

## Phase B (3-6 months): Regulated beta
Goal: run controlled pilots with real counterparties and audit-ready operations.

Technical deliverables:
1. Multi-venue adapters
- FIX + WebSocket market/order adapters, normalized schema, and failover routing.
2. Smart order routing v1
- Venue selection by top-of-book + fee/latency score.
3. Market simulator and paper trading
- Slippage, queue position, and latency modeling.
4. Compliance core
- Immutable audit trail, retention policy, suspicious behavior heuristics (spoofing/wash-trading signals).
5. Security controls
- RBAC, token lifecycle management UI, MFA for admin operations, TLS hardening baseline.
6. Operability
- Prometheus/Grafana dashboards, SLOs, alerting, runbooks, incident response templates.

Exit KPIs:
- Stable 30-day pilot uptime target.
- Full audit reconstruction for any selected order id.
- No Sev-1 incidents unresolved beyond agreed SLA.

## Phase C (6-12 months): Enterprise scale and moat
Goal: be defensible for larger institutional contracts and funding rounds.

Technical deliverables:
1. Distributed architecture
- Split services (feed, risk, OMS, API), move from in-proc to distributed event backbone where justified.
2. HA and disaster readiness
- Active-active or active-standby by service tier, documented failover drills.
3. Compliance pack by jurisdiction
- Configurable reporting exports and policy packs for target regions.
4. MLOps productionization
- Dataset/version registry, offline/online evaluation, controlled model rollout.
5. Commercial packaging
- Multi-tenant controls, usage metering, support tiers, and enterprise license workflows.

Exit KPIs:
- Customer reference accounts in production.
- Audit/compliance readiness package accepted by pilot clients.
- Demonstrated cost-to-serve and margin profile for scale.

## 4) Funding strategy tied to milestones

Use milestone-based financing rather than a single broad raise.

1. Pre-seed / bridge (now -> Phase A)
- Use of funds: product hardening, deterministic replay, API completeness, risk controls.
- Narrative: "prototype to institutional pilot-ready core".

2. Seed (after Phase A evidence)
- Use of funds: multi-venue integration, compliance core, pilot operations.
- Narrative: "from pilot-ready engine to regulated beta with paying design partners".

3. Series A readiness (after Phase B)
- Use of funds: scale architecture, enterprise sales, cross-jurisdiction compliance modules.
- Narrative: "B2B trading infrastructure with repeatable deployments and defensible unit economics".

## 5) Non-negotiable investor diligence artifacts

Prepare these artifacts before major fundraising:

1. Architecture and ADR set (current + target state).
2. Latency benchmark methodology with reproducible scripts.
3. Security baseline and penetration test plan.
4. Audit trail/replay demonstration package.
5. Customer pilot agreement template and SLA draft.
6. Pricing model with support tiers and estimated gross margin.

## 6) Immediate 30-day execution (recommended)

1. Freeze v1 order-state machine and event schema.
2. Implement append-only event journal + replay CLI.
3. Add cancel/replace and list-orders APIs.
4. Add account/instrument risk snapshots and kill switch API.
5. Build dashboard panels for latency/risk/order lifecycle.
6. Add CI gates for replay determinism and API contracts.

This 30-day block is the highest ROI path from "interesting prototype" to "fundable technical product".
