# Fase 07 - Backtesting

## Objetivo

Build serious backtesting that measures strategy behavior with realistic costs and avoids common biases.

## Estado Actual

Pre-existing (Block 0/1): Mode A — equity-curve analysis (PnL, max drawdown,
Sharpe) over already-executed fills loaded from CSV/journal.

Block 2 (2026-07) delivered, with tests:

- **Metrics refactor**: the equity-curve math is now a pure, hand-testable
  function (`compute_equity_curve_metrics`, `backtest/backtest_metrics.hpp`)
  shared by Mode A (public output unchanged), Mode B and the bootstrap.
- **Sortino** (MAR=0 target semideviation, same sqrt(252) annualization as
  the existing Sharpe) and **historical VaR/CVaR 95**
  (`risk/historical_var.hpp`, empirical, positive-loss convention — chosen
  over the unused parametric `var_calculator.hpp` because strategy returns
  are fat-tailed).
- **Monte Carlo bootstrap** (`run_monte_carlo_bootstrap` + engine wrapper):
  i.i.d. fill resampling, seeded and bit-deterministic, per-metric P5/P50/P95.
  Explicitly documented as a robustness bound, not scenario simulation.
- **Mode B — counterfactual runs** (`BacktestEngine::run_strategy`): drives
  the EXACT production decision path (SignalEngine -> EVGate -> OrderManager
  -> RiskManager -> OrderBook matching) over historical ticks, with bar
  aggregation -> regime classification stamping every candidate, synthetic
  maker liquidity seeded at the reference price, risk day-roll and
  mark-to-market per tick, and fills reconstructed from the run's own journal
  (exercising the audit path). Reports funnel + metrics + regime bar counts.
- **In-memory tick loading** (`load_ticks`) for tests and synthetic studies.
- End-to-end harness: `argentum_backtest_demo` (CSV first, deterministic
  synthetic fallback) printing funnel, regime distribution, metrics and
  Monte Carlo percentiles.
- See `docs/ADR/0012-regime-classifier-and-counterfactual-backtesting.md`
  for the design decisions (including why Mode B does not use
  `MarketExecutionSimulator`).

## Problemas Detectados (restantes)

- Synthetic liquidity fills AT the reference tick price: no spread/depth
  model (documented limitation; cost realism is explored via
  `run_cost_sensitivity` instead — a depth model joins the Block 5+
  multi-venue work if evidence demands it).
- No look-ahead/survivorship hard gates yet; single-symbol runs only.
- Backtest reports are printed/returned, not yet persisted with strategy and
  data versions (joins the Java reporting service work, Block 5/6).

## Subfases

- [x] Define historical dataset contracts (CSV/journal loaders + `load_ticks`).
- [x] Add cost and slippage model interfaces (via the EV gate `CostModel`,
      shared between live gating and backtest).
- [x] Add out-of-sample and walk-forward workflow (Block 3:
      `run_walk_forward` with a strategy FACTORY — fresh instance per fold so
      internal state cannot leak across out-of-sample windows; aggregate OOS
      metrics over chronologically concatenated fold fills).
- [x] Add strategy report outputs (`StrategyBacktestReport`: funnel, metrics,
      regime distribution, per-regime evaluation report, lifecycle
      transitions, final health snapshot).

## Tareas Tecnicas

- [x] Track trades, equity curve, drawdown, returns (pure metrics function).
- [x] Add validation windows (walk-forward folds; train windows recorded but
      inert until Block 4+ model retraining — this is OOS evaluation, not
      hyperparameter optimization, and is documented as such).
- [x] Add stress and Monte Carlo hooks (bootstrap + cost-shock multipliers +
      the Stress bucket of the per-regime report; no fabricated synthetic
      crashes — see ADR 0013).
- [ ] Store backtest reports with strategy and data versions (Block 5/6).

## Criterios de Aceptacion

- [ ] Backtest cannot run without explicit data range and cost assumptions
      (cost model is explicit; date-range enforcement pending).
- [x] Reports include PnL, drawdown, Sharpe-like metrics, costs and trade
      attribution (funnel + per-reason rejects + journal correlation +
      per-regime realized-vs-predicted).
- [x] Strategy promotion requires out-of-sample evidence (walk-forward
      aggregate OOS metrics available; promotion gating itself is a Block 4+
      model-registry workflow).

## Metricas Esperadas

- [x] Total PnL; net-of-cost by construction in Mode B (fills execute against
      seeded liquidity through the real matching path).
- [x] Max drawdown, Sharpe, Sortino.
- [x] Historical VaR/CVaR 95 over per-trade equity changes.
- [x] Monte Carlo P5/P50/P95 per metric.
- [x] Signal funnel (candidates, accepted, rejects by reason) per run.

## Tests Requeridos

- [x] Cost model tests (`ev_gate_test`, Block 1).
- [x] Deterministic replay tests (`backtest_metrics_test`: bit-identical
      bootstrap for a fixed seed).
- [x] Known dataset expected-result tests (`backtest_metrics_test`:
      hand-computed Sharpe/Sortino/VaR/CVaR/drawdown).
- [x] Counterfactual pipeline test (`backtest_run_strategy_test`: regime
      stamping proven via allowed-mask exclusion, funnel arithmetic, journal
      correlation).
- [ ] No-look-ahead tests (Block 3, with windowing).

## Benchmarks Requeridos

- [x] Regime classification latency (`argentum_regime_benchmark`: p50 ~600ns,
      p99 ~900ns per bar on the reference machine, Release).
- [ ] Backtest runtime per million events (formalize once walk-forward lands).

## Riesgos Tecnicos

- Decorative backtests can mislead product and funding decisions — the demo
  strategy remains explicitly labeled a machinery exercise, and the Monte
  Carlo output prints its own honesty note.
- Bias controls are easy to skip without hard gates — pending Block 3.

## Dependencias

- Persistence and replay (in place; Mode B reads its own journal).
- Strategy and signal versioning (model_version required per candidate since
  Block 1).

## Resultado Esperado

A backtesting engine useful for real strategy validation, not only
demonstration. **Status: functionally complete for the Phase-1 scope
(crypto/equities, single venue) — counterfactual core, institutional
metrics, Monte Carlo, walk-forward OOS, per-regime evaluation report and
cost-sensitivity/stress sweeps all delivered and tested (Blocks 2-3).
Remaining: report persistence/versioning and date-range hard gates.**
