# ADR 0011: EV-Gated Signal Engine

## Status

Accepted (implemented in Block 1, 2026-07).

## Context

The platform promises "EV-gated execution": trades are only accepted when net
expected value is positive after modeled costs, conditioned by market regime
and strategy lifecycle state, with a full explainability audit trail. Before
this ADR, none of that existed in code — strategies were an interface plus an
SMA helper, and nothing sat between a strategy and `OrderManager::submit_order()`.

Constraints that shaped the design:

- The existing hot path (order book matching, risk checks, journal ring buffer)
  must not gain latency or new dependencies.
- `RiskManager` remains the final capital defense; signal quality filtering is
  a separate, upstream concern.
- A solo developer budget: the design must be testable in isolation and grow
  incrementally (regime classifier in Block 2, statistical lifecycle registry
  in Block 3, ML-served inputs in Block 4).

## Decision

Three new static libraries, layered so math, regime and orchestration stay
independent:

| Library | Namespace | Responsibility |
| --- | --- | --- |
| `argentum_ev` | `argentum::ev` | Pure EV math + cost model + acceptance gate. Stateless, no I/O. |
| (header-only) | `argentum::regime` | `RegimeLabel` contract consumed by the gate; classifier lands in Block 2. |
| `argentum_signal` | `argentum::signal` | Orchestrator: candidate -> lifecycle -> EV gate -> OMS, plus audit emission. |

### EV formula

```
ev_gross_bps = p_win * avg_win_bps + (1 - p_win) * avg_loss_bps
cost_bps     = 2*taker_fee + 2*half_spread + slippage + market_impact
             + funding_bps_per_hour * expected_holding_hours
ev_net_bps   = ev_gross_bps - cost_bps
```

Acceptance requires `ev_net_bps >= min_ev_bps_threshold * regime_multiplier`,
`confidence >= min_confidence`, an allowed regime, and a non-blocked lifecycle
state. The threshold floor is deliberately above zero: marginally positive EV
inside the estimation noise of `p_win` is not a tradeable edge.

### Fail-closed rule

Malformed inputs (NaN, out-of-range probability, non-positive notional,
negative costs, unknown regime enum) reject with `invalid_inputs`. When the
future Python model bridge times out, callers must reject with
`ml_bridge_unavailable` — never substitute a default prediction. This reverses
the previous `PythonBridge` stub behavior (`return 0.5`).

### Lifecycle enforcement

`StrategyState` (`active` / `under_observation` / `quarantined` / `disabled`)
is enforced at the gate: quarantined/disabled block, under-observation trades
at a configurable size haircut (default 25%) — graceful degradation rather
than a binary switch. The state registry with statistical transitions (CUSUM
on realized PnL, rolling Sharpe floor, EV-decay t-stat) is Block 3 work; the
enforcement semantics are fixed now so they never change underneath it.

### Audit trail

Every decision emits a `signal_decision` structured event through the existing
async `audit::Logger` (JSON lines in `audit.log`): strategy id, model version,
feature snapshot reference, regime + confidence, p_win/avg_win/avg_loss, every
cost component, gross/cost/net EV, threshold, accept/reject reason, resulting
order id, and a human-readable `decision_reason`. `JournalEvent` gained a
backward-compatible `related_signal_id` field (absent in old files parses as
0), and `OrderManager::submit_order()` takes a defaulted `related_signal_id`
parameter, so replay can reconstruct order -> signal -> reasoning end to end.

### Risk hardening delivered alongside

`RiskLimits::max_daily_loss` (previously declared but dead) is now enforced:
per-symbol average-cost position accounting, realized + marked-to-market
unrealized PnL, a UTC day-roll baseline, and an atomic kill switch that blocks
`check_order()` while still accounting in-flight fills. The kill switch is
audited on trigger/reset and is never reset by a day roll — re-enabling
trading after a breach is a manual operator decision.

## Consequences

- The decision layer is exercised end to end by `argentum_signal_demo`
  (synthetic deterministic ticks, SMA-crossover demo strategy) and covered by
  `ev_gate_test`, `signal_engine_test`, `risk_daily_loss_test`,
  `risk_kill_switch_test`.
- Gate evaluation measured at p50 ~100ns / p99 ~300ns per call (Release, MSVC,
  demo workload) — negligible against the < 25ms decision budget.
- The order hot path is untouched: the gate runs before `submit_order()`, and
  all journal changes are additive fields.
- Regime input is `Unknown` until Block 2; strategies must opt into whether
  `Unknown` is tradeable via `allowed_regimes_mask`.
