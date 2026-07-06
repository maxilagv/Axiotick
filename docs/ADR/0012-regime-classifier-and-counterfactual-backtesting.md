# ADR 0012: Regime Classifier and Counterfactual Backtesting (Mode B)

## Status

Accepted (implemented in Block 2, 2026-07).

## Context

Block 1 delivered the EV gate with regime hooks (`RegimeLabel`, per-regime
threshold multipliers, allowed-regime masks) but every candidate carried
`Unknown` because no classifier existed. Backtesting could only recompute
metrics over already-executed fills (Mode A) — it could not answer "what
would this strategy have done", which is what strategy validation requires.

## Decisions

### 1. Bar aggregation is a pure class, not a bus consumer

`regime::BarAggregator` (`backend/include/regime/bar_aggregator.hpp`) is pure
computation: tick in, optional closed bar out. The same instance semantics
serve the backtest loop today and the live bus subscriber in Block 4 — same
class, same numbers, no drift between offline and online features. Fixed
semantics: boundary-aligned bars; the crossing tick opens the next bar; no
synthetic empty bars across gaps; stale ticks ignored.

### 2. Rule-based classifier now, statistically calibrated thresholds later

`regime::RegimeClassifier` uses deterministic threshold rules over rolling
features (realized vol, Wilder ADX/ATR, ATR percentile vs. own history with
midrank tie handling, volume z-score computed against the window excluding
the current bar). Rule priority is fixed: **Stress > VolExpansion > Trend >
Range > Unknown** — safety first: a violent directional crash satisfies both
Stress and Trend, and must classify as Stress. Confidence is the saturating
margin of the decisive metric (excess/(1+excess), always < 1).

The HMM/GMM work scheduled for Block 6 calibrates these thresholds offline in
Python and ships them as a versioned JSON; the rule structure and this class
do not change. Declaring `Unknown` in ambiguous zones is deliberate: the EV
gate decides per strategy whether Unknown is tradeable (`allowed_regimes_mask`).

### 3. Mode B does NOT use MarketExecutionSimulator (correction to the master plan)

The master plan suggested reusing `gateway::MarketExecutionSimulator` for
counterfactual fills. Inspection showed its API solves a different problem:
multi-venue routing simulation (`RoutingDecision` + `VenueOrderBookSnapshot`
inputs, queue-position/latency modeling per venue). Forcing a single-symbol
historical backtest through the routing machine would be complexity without
benefit.

Instead, `BacktestEngine::run_strategy` formalizes the pattern the Block 1
demo already proved: **seed a synthetic maker order into the real
`OrderBook` at the reference tick price, submit the candidate through the
real `SignalEngine -> EVGate -> OrderManager -> RiskManager -> matching`
path, then cancel the maker remainder.** The backtest therefore executes the
EXACT production decision code — the strongest anti-drift argument a
backtester can make. `MarketExecutionSimulator` remains the right tool for
Block 5+ multi-venue/best-execution studies, where its complexity pays.

Known v1 limitation (documented in `StrategyBacktestConfig`): synthetic
liquidity fills AT the reference price with no spread/depth model. Realistic
liquidity shaping joins the Block 3 cost-sensitivity work.

### 4. Metrics are pure functions; Monte Carlo is an honest bootstrap

The equity-curve math moved out of `BacktestEngine::run()` into
`compute_equity_curve_metrics` (`backend/include/backtest/backtest_metrics.hpp`)
— one implementation shared by Mode A (whose public output is unchanged),
Mode B, the bootstrap and the tests. Additions: Sortino (MAR=0 target
semideviation over all n returns, annualized with the same sqrt(252)
convention as the existing Sharpe) and historical VaR/CVaR 95
(`backend/include/risk/historical_var.hpp`, empirical percentile + tail mean,
positive-loss convention) — chosen over the pre-existing parametric
`var_calculator.hpp` because fat-tailed strategy returns violate its
normality assumption; the parametric file stays as an unused alternative
until Block 6 decides its fate.

`run_monte_carlo_bootstrap` resamples fills i.i.d. with replacement
(seeded `mt19937_64`, bit-deterministic) and reports per-metric P5/P50/P95.
Its docstring states plainly: this is a robustness bound on sample
composition/ordering, **not** a market-scenario simulation — resampling
destroys temporal autocorrelation, and position-dependent metrics (equity
VaR) widen accordingly.

### 5. Fills are reconstructed from the run's journal, not returned in memory

`run_strategy` scores the run by re-reading `trade_executed` events (with
`related_signal_id`) from its own scratch journal. Slightly slower than
threading fills through memory, chosen deliberately: every Mode B run also
exercises the audit/replay path end to end.

## Consequences

- New library `argentum_regime`; `argentum_backtest` now links
  `argentum_signal`/`argentum_regime` (it drives the production path).
- `CandidateStrategy` (`backend/include/signal/candidate_strategy.hpp`)
  formalizes the strategy contract; the SMA demo strategy moved to
  `backend/include/analysis/sma_crossover_strategy.hpp` and is shared by
  `argentum_signal_demo` and `argentum_backtest_demo` unchanged.
- Classifier latency (Release, defaults): p50 ~600ns, p99 ~900ns per bar —
  four orders of magnitude under the ms budget; bars arrive at
  seconds-to-minutes cadence.
- Tests: `bar_aggregator_test`, `regime_classifier_test` (including the
  Stress-beats-Trend priority case), `backtest_metrics_test` (hand-computed
  Sortino/VaR/CVaR, bit-determinism of the bootstrap),
  `backtest_run_strategy_test` (proves regime stamping by excluding Unknown
  from the allowed mask: pre-warmup candidates reject `regime_blocked`,
  post-warmup ones pass).
