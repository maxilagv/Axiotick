# Fase 04 - Signal Engine

## Objetivo

Create a signal engine based on probabilistic evidence, expected value and traceable inputs.

## Estado Actual

Block 1 (2026-07) delivered the core of this phase. Implemented and tested:

- `argentum_ev` (`backend/modules/ev/`, `backend/include/ev/`): stateless EV
  gate with the net-EV formula (p_win/avg_win/avg_loss vs. fees, spread,
  slippage, market impact, funding x holding time), a configurable threshold
  floor above zero, per-regime threshold multipliers and an allowed-regimes
  mask. Fail-closed on malformed inputs.
- `argentum_signal` (`backend/modules/signal/`): `SignalEngine` orchestrating
  candidate -> lifecycle state -> EV gate -> `OrderManager::submit_order()`,
  with funnel stats and per-call gate latency percentiles.
- Explainability payload: every decision (accepted or rejected) emits a
  `signal_decision` structured audit event with inputs, model version, feature
  snapshot reference, cost components, EV breakdown, accept/reject reason and
  the resulting order id. Orders are journaled with `related_signal_id`.
- Lifecycle enforcement at the gate: `active` / `under_observation` (25% size
  haircut) / `quarantined` / `disabled` (blocked). Block 3 delivered the
  statistical registry that drives automatic transitions
  (`strategy::StrategyRegistry`): Page-Hinkley break detection on
  horizon-evaluated results, rolling-Sharpe floor vs. registered baseline,
  EV-decay t-stat, strategy-level drawdown, recovery streaks and automatic
  Disable on repeated quarantines — every transition audited as
  `strategy_transition`. See `docs/ADR/0013-strategy-lifecycle-governance.md`.
- End-to-end harness: `argentum_signal_demo` (SMA-crossover demo strategy over
  deterministic synthetic ticks) exercising gate accept/reject, OMS fills,
  daily-loss kill switch and the audit trail.
- Tests: `ev_gate_test`, `signal_engine_test` (plus risk tests in Fase 05).

See `docs/ADR/0011-ev-gated-signal-engine.md` for the full design decision.

## Problemas Detectados (restantes)

- p_win / expected-move inputs are strategy-supplied heuristics; calibrated
  model-served inputs (Python bridge with fail-closed timeout) land in Block 4.
- Signal schema is JSON-in-audit-log; a versioned binary schema can follow
  once the Java reporting service defines its consumption contract.
- Regime thresholds are hand-set engineering conventions; HMM/GMM-calibrated
  versioned thresholds arrive in Block 6.

## Subfases

- [x] Define signal event schema (`signal/signal_types.hpp` + audit payload).
- [x] Implement feature input contracts (`ev/ev_types.hpp::EVInputs`).
- [x] Add EV and risk-adjusted signal output (`ev/ev_gate.hpp`).
- [x] Add signal explanation and version metadata (audit `signal_decision`).
- [x] Regime-conditioned inputs: Block 2 delivered `regime::BarAggregator` +
      `regime::RegimeClassifier` (Stress > VolExpansion > Trend > Range >
      Unknown, saturating-margin confidence); the backtest runner stamps
      every candidate, and the live pipeline reuses the same classes in
      Block 4. Latency: p50 ~600ns / p99 ~900ns per bar
      (`argentum_regime_benchmark`). See ADR 0012.
- [ ] Model-served inputs via the real Python bridge (Block 4).

## Tareas Tecnicas

- [x] Separate signal generation from order creation.
- [x] Include costs, spread, slippage and confidence in signal payloads.
- [x] Add rejection reasons when signals are not actionable
      (`invalid_inputs`, `lifecycle_blocked`, `regime_blocked`,
      `confidence_below_min`, `ev_below_threshold`, `ml_bridge_unavailable`).
- [x] Add strategy quarantine hooks for degraded signals — gate-side
      enforcement (Block 1) + automatic statistical transitions (Block 3:
      CUSUM/Sharpe-floor/EV-decay/drawdown driving the state the gate reads).

## Criterios de Aceptacion

- [x] Every signal has inputs, version and explanation.
- [x] Signal latency meets configured strategy window (gate p50 ~100ns,
      p99 ~300ns per evaluation in Release demo runs — budget is ms-scale).
- [x] No signal directly bypasses risk or OMS (gate submits through
      `OrderManager::submit_order()`, which always runs `RiskManager`).

## Metricas Esperadas

- Signal latency p50/p95/p99: exposed via `SignalEngine::gate_latency_snapshot()`.
- Signal count, accepted count and rejected count: `SignalEngine::stats()`
  (candidates, gate_accepted, orders_submitted, orders_accepted,
  rejects_by_reason).
- EV distribution and confidence distribution: derivable from the
  `signal_decision` audit stream (numeric fields are logged per decision).

## Tests Requeridos

- [x] Feature contract tests (`ev_gate_test`: input validation, fail-closed).
- [x] EV calculation tests (`ev_gate_test`: exact gross/cost/net math).
- [x] Signal traceability tests (`signal_engine_test`: journal carries
      `related_signal_id` for accepted orders and their fills).

## Benchmarks Requeridos

- [x] Regime classification latency benchmark (`argentum_regime_benchmark`,
      p50/p95/p99/p99.9 over 1M bars).
- Per-call gate latency is measured in the demos; a standalone gate benchmark
  can join the pipeline benchmark suite when the Block 4 live path lands.
- Batch recalculation benchmark for 1s, 5s and 1m strategies: bar aggregation
  now exists; formalize alongside the Block 4 live feeds.

## Riesgos Tecnicos

- Decorative signals can create false product value — the demo strategy is
  explicitly labeled a machinery exercise, not an alpha claim.
- Unversioned signals cannot be audited — `model_version` is a required field
  of every candidate.

## Dependencias

- Market data schema (existing tick schema is sufficient for Block 1).
- Feature/context design (Block 2: bar aggregator + regime features).

## Resultado Esperado

A first serious signal pipeline that can be validated and audited. **Status:
delivered for rule-based inputs; regime conditioning and model-served inputs
remain on the Block 2/4 schedule.**
