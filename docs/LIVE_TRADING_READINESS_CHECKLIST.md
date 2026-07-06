# Live Trading Readiness Checklist

Live trading is prohibited until this checklist is satisfied for the target strategy, venue and deployment environment.

## Build and Release

- Release build is reproducible from a clean checkout.
- All relevant unit, integration and contract tests pass.
- Versioned artifact is tagged and traceable to source.
- Configuration is reviewed and environment-specific.
- Rollback procedure is documented and tested.

## Risk Controls

- Every order path passes pre-trade risk checks.
- Max order value, max exposure, symbol limits and account limits are configured.
- Daily loss limit and drawdown guardrails are active.
- Kill switch is tested and available to authorized operators.
- Risk ledger is reconciled against OMS state.
- Rejected orders include explicit reject reasons.

## Strategy Validation

- Strategy hypothesis is documented.
- Backtest includes fees, spread, slippage and funding where relevant.
- Out-of-sample and walk-forward results are reviewed.
- Stress tests and Monte Carlo analysis are complete.
- Strategy degradation and quarantine rules are defined.
- Expected value is positive after estimated costs.

## Paper Trading

- Strategy has completed an agreed paper-trading window.
- Paper fills include realistic latency and slippage assumptions.
- Paper results are reconciled against expected backtest behavior.
- Failure modes and operator procedures are tested.

## Observability

- Latency metrics are captured for market data, signal, risk, OMS and execution.
- p50, p95, p99 and p99.9 are visible where relevant.
- Drops, backpressure, rejects and errors are monitored.
- Alerts exist for stale data, disconnected venues and risk limit breaches.
- Order lifecycle can be reconstructed from audit events.

## Security

- API keys are stored securely and never committed.
- Read-only and trading permissions are separated.
- Production tokens are rotated and revocable.
- Operator access is role-based.
- TLS and network boundaries are reviewed.
- Withdrawal permissions are disabled or separately controlled where applicable.

## Execution and Venue

- Venue connector has been tested in sandbox or paper mode.
- Rate limits and retry behavior are understood.
- Order types and time-in-force semantics are confirmed.
- Disconnect, partial fill, reject and duplicate acknowledgement cases are tested.
- Execution gateway has an emergency stop path.

## Approval

- Technical owner approves build and deployment.
- Risk owner approves limits and strategy.
- Operator owner approves runbook and monitoring.
- Live trading window, maximum capital and rollback criteria are documented.
