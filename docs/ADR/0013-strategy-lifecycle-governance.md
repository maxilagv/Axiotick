# ADR 0013: Strategy Lifecycle Governance and Advanced Backtest Validation

## Status

Accepted (implemented in Block 3, 2026-07).

## Context

Blocks 1-2 delivered the EV gate (which already ENFORCED lifecycle states)
and the regime-conditioned counterfactual backtester — but nothing DECIDED
lifecycle states except a manual setter. "Strategy lifecycle: each strategy
monitored for statistical degradation" was enforcement without governance.
Block 3 closes the loop with `strategy::StrategyRegistry`
(`backend/modules/strategy/`) and completes the backtesting phase with the
per-regime report, walk-forward validation and cost-sensitivity sweeps.

## Decisions

### 1. Attribution by horizon evaluation, NOT position tracking

The obvious design — a per-strategy position ledger mirroring
`RiskManager::PositionState` — breaks as soon as one signal opens a position
and a different signal closes it (exactly what an SMA-crossover does: the
bearish cross closes the long AND opens the short in the same fill).
Attributing that realized PnL back to originating signals would require
FIFO/LIFO lot tracking per signal — real accounting complexity that no
current use case needs.

Instead, every accepted fill is scored against the signal's OWN declared
horizon (`EVInputs.costs.expected_holding_hours`): when the horizon elapses,
`realized_bps = (price_at_horizon / entry - 1) * 1e4 * side_sign`. This
compares exactly what the signal promised ("ev_net_bps over H hours")
against what the market did — independent of what other signals did to the
shared position, and it is precisely the input the EV-decay test needs.
Consequences: pending evaluations live in a `std::multimap` keyed by
resolve-time; ticks resolve due evaluations per symbol; `flush_pending` at
backtest end resolves un-expired samples instead of silently dropping them.
FIFO/LIFO lot tracking is deferred until concurrent multi-strategy trading
on one symbol exists.

### 2. Page-Hinkley in the downward-shift form (correction to the plan sketch)

The planning sketch wrote the PH recursion with a min-tracker and
`(x - mean - delta)`, which is the mirrored INCREASE detector and does not
fire on decay. The implementation uses the standard decrease form:

```
mean_t = running mean (Welford)
m_t    = m_{t-1} + (x_t - mean_t + delta)
M_t    = max(M_{t-1}, m_t)
PH_t   = M_t - m_t            -> alarm when PH_t > lambda
```

A winning streak drifts `m` upward with `M` tracking it (PH = 0); a sustained
mean drop beyond `delta` sinks `m` while `M` remembers the peak, so PH grows.
`strategy_registry_cusum_test` encodes hand-computed values of this form
(PH = 4.9545... one evaluation before the alarm, alarm on the next). After
any transition the detector resets so the NEXT break is detectable; the
rolling windows are deliberately NOT reset — recovery must be earned by the
window actually healing, not by amnesia.

### 3. Transition machine

```
Active -> UnderObservation:      PH alarm OR rolling-Sharpe floor breach
UnderObservation -> Active:      N consecutive clean evaluations
UnderObservation -> Quarantined: strategy drawdown breach OR EV-decay t-stat
Active -> Quarantined:           drawdown breach (capital protection does not
                                 wait for statistical confirmation)
Quarantined -> Active:           manual force_state only
Quarantined -> Disabled:         automatic on the Nth quarantine within the
                                 lookback (the only automatic Disable path)
```

The Sharpe floor is a RATIO against a registered backtest baseline; with no
baseline (0) it is disabled by design — there is nothing to degrade from.
The EV-decay test is a one-sided t-stat of (realized - predicted) with a
minimum-sample gate. Thresholds are documented engineering conventions
(recalibrate `lambda ~ 2-3x` the strategy's own backtest stddev), same
spirit as ADX=25 in ADR 0012. Every transition — automatic or manual — is
audited as a `strategy_transition` structured event.

`SignalEngine` embeds the registry: fills from `submit_order` (previously
discarded) feed `record_signal_open`; `on_market_tick` resolves horizons;
the EV gate reads `registry().state()` — so a quarantine decided by
statistics blocks the very next candidate. The Block 1 manual API
(`set_strategy_state`) now delegates to `force_state` unchanged.

### 4. Per-regime report from resolved evaluations, not filtered fills

Filtering fills by regime and re-running the equity engine per subset breaks
accounting whenever a position opens in one regime and closes in another
(phantom legs, misattributed PnL). The report instead aggregates the
horizon-evaluation stream — each sample is self-contained and carries the
regime AT SIGNAL TIME. The demo output shows why this matters: on the
synthetic series the SMA strategy's promise holds in Trend (realized 67bps
vs 24 predicted) and inverts in Stress (realized -80bps vs +52 predicted) —
the "per-regime stability" evidence the platform promises, computed
correctly.

### 5. Walk-forward requires a strategy FACTORY

Reusing one strategy instance across folds silently invalidates
out-of-sample independence: internal state (moving averages, confirmation
counters) from fold N contaminates fold N+1. `run_walk_forward` therefore
takes a `StrategyFactory` and instantiates per fold;
`backtest_walk_forward_test` proves the property with a strategy that only
emits on its first K ticks (a reused instance would emit 3 candidates total;
fresh instances emit 3 per fold). Train windows are recorded but inert:
rule-based strategies do not retrain — this is out-of-sample evaluation,
NOT hyperparameter optimization, and must not be presented as such.

### 6. Cost sensitivity is a decorator, and sweeps disarm governance

`analysis::CostScaledStrategy` scales only the bps cost components (never
probabilities or the horizon). The sweep runs fresh instances per
multiplier. Governance is disarmed inside the sweep: with lifecycle armed,
different cost levels change WHICH candidates trade, which changes WHEN
quarantine hits — the sweep would measure lifecycle interaction, not costs.
A sensitivity analysis varies one factor. Stress testing composes existing
tools (the Stress bucket of the regime report + extreme cost multipliers);
no synthetic price crashes are fabricated — that would be exactly the
decorative evidence the project's audit discipline forbids.

## Consequences

- New library `argentum_strategy`; `argentum_signal` links it;
  `SignalEngine::Config` gains a registry config (defaults preserve all
  Block 1/2 behavior — 38/38 tests pass, 30 of them pre-existing).
- `StrategyBacktestReport` gains `by_regime`, `lifecycle_transitions`,
  `final_health`, `resolved_evaluations`; `HistoricalTrade` gains
  `related_signal_id` (parsed from the journal written since Block 1).
- Known calibration coupling, documented on the demo: horizon length must be
  sized to the tick density of the data, or horizon evaluations measure
  multi-thousand-bps moves that mean nothing at the declared EV scale.
- Cross-check that fell out for free: the cost sweep at x1.0 with governance
  off reproduces the Block 2 demo PnL exactly (-144.06), isolating
  governance as the only behavioral delta of this block.
