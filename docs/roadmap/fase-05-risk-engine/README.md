# Fase 05 - Risk Engine

## Objetivo

Upgrade risk from basic order/exposure checks into a deterministic pre-trade and runtime control system.

## Estado Actual

Pre-existing: order validity, max order value, global exposure with atomic
reservation/release accounting, and per-symbol net/gross limits.

Block 1 (2026-07) added, with tests:

- **Daily loss limit + automatic kill switch**: `RiskLimits::max_daily_loss`
  (previously declared but unused) is now enforced. Per-symbol average-cost
  position accounting produces realized PnL on reducing/closing/flipping
  fills; `mark_to_market()` refreshes unrealized PnL; a breach of
  `-(max_daily_loss)` triggers the kill switch automatically with the exact
  reason audited (`risk_kill_switch_triggered`).
- **Kill switch semantics**: `check_order()` rejects while active (checked
  first, one atomic load); in-flight fills are still accounted so position
  truth is never corrupted; `trigger_kill_switch()` / `reset_kill_switch()`
  are idempotent and audited; a UTC day roll (`maybe_roll_day()`) rebases the
  daily PnL baseline but never re-enables trading — reset is a manual
  operator decision.
- **PnL surfaces**: `daily_pnl()`, `realized_pnl()`, `unrealized_pnl()`,
  ready for the future Java risk-snapshot service to poll.
- Tests: `risk_daily_loss_test` (realized breach, unrealized breach via mark,
  average-cost math incl. position flip, day-roll rebasing),
  `risk_kill_switch_test` (manual trigger/reset, idempotency, day-roll does
  not reset, zero-limit disables auto-trigger). Existing reservation and
  symbol-limit tests unchanged and passing.

## Problemas Detectados (restantes)

- VaR/CVaR is still not computed at runtime (`var_calculator.hpp` remains
  unreferenced); historical VaR/CVaR on a rolling portfolio-returns buffer is
  scheduled with the backtesting extensions (Block 2).
- Correlation/cluster exposure caps are not implemented (planned as static
  configurable clusters, Block 6).
- Risk decisions are logged but not yet assigned decision ids on order events.

## Subfases

- [x] Freeze risk ledger model (reservation ledger pre-existing; position/PnL
      ledger added).
- [x] Add position, PnL and limit snapshots (accessor surface).
- [x] Add drawdown/daily loss and kill switch controls (daily loss + kill
      switch delivered; strategy-level drawdown belongs to the Block 3
      lifecycle registry).
- [ ] Add VaR/CVaR and correlation roadmap (Blocks 2 and 6).

## Tareas Tecnicas

- [x] Track reserved, filled, released exposure separately (pre-existing) and
      realized/unrealized PnL separately (new).
- [ ] Attach risk decision ids to order events.
- [x] Add symbol risk scopes (pre-existing); account/venue scopes pending.
- [x] Add operator-visible risk snapshots (PnL/kill-switch accessors; the
      HTTP surface arrives with the Java service in Block 5/6).

## Criterios de Aceptacion

- [x] No order can reach execution without risk approval (unchanged
      invariant; kill switch adds a global block).
- [ ] Risk replay reconstructs final risk state (journal replay reconstructs
      exposure; PnL replay pending).
- [x] Kill switch blocks new orders and is audited.

## Metricas Esperadas

- Risk check latency: unchanged hot path + one atomic load.
- Risk rejects by reason: logged (`kill switch active`, order value,
  exposure, symbol limits).
- Current exposure, reserved and filled exposure: pre-existing accessors.
- Daily PnL and drawdown: `daily_pnl()` live; drawdown series pending.

## Tests Requeridos

- [x] Reservation invariant tests (pre-existing).
- [x] Partial fill/cancel/replace tests (pre-existing).
- [x] Kill switch tests (`risk_kill_switch_test`).
- [ ] Replay risk reconstruction tests including PnL.

## Benchmarks Requeridos

- Risk check p50/p95/p99/p99.9: pending formal benchmark target.
- High-order-count risk ledger benchmark: pending.

## Riesgos Tecnicos

- Incorrect risk release can create hidden exposure (covered by existing
  reservation tests).
- Average-cost PnL uses doubles; acceptable for limit enforcement, but a
  fixed-point ledger should be evaluated before live trading gates.

## Dependencias

- OMS state machine (existing).
- Event journal schema (extended with `related_signal_id` in Block 1).

## Resultado Esperado

A risk engine credible enough for paper trading and later live readiness
gates. **Status: daily-loss/kill-switch layer delivered; VaR/CVaR and
correlation caps remain scheduled.**
