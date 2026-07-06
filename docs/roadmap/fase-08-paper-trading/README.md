# Fase 08 - Paper Trading

## Objetivo

Run strategies against live or replayed market data with simulated execution, realistic latency and full risk controls.

## Estado Actual

Paper trading is not a complete platform mode yet.

## Problemas Detectados

- Simulated fills, slippage and queue position need explicit modeling.
- Paper and live execution boundaries must be impossible to confuse.
- Operator workflows need visibility into paper orders and risk.

## Subfases

- Define paper account model.
- Add venue simulator with latency and slippage profiles.
- Connect strategy -> risk -> OMS -> simulator path.
- Add paper performance reports.

## Tareas Tecnicas

- Simulate partial fills, rejects, disconnects and stale market data.
- Track paper positions, PnL, costs and drawdown.
- Add paper account reset and report export.
- Add UI/API controls for paper mode only.

## Criterios de Aceptacion

- Paper orders never reach live connectors.
- Paper results are replayable and auditable.
- Risk controls apply exactly as they would for live mode.

## Metricas Esperadas

- Simulated execution latency.
- Fill ratio.
- Slippage distribution.
- Paper PnL and drawdown.
- Risk rejects and kill-switch events.

## Tests Requeridos

- Simulator fill tests.
- Paper/live separation tests.
- Risk enforcement tests in paper mode.
- Replay of paper sessions.

## Benchmarks Requeridos

- Paper execution throughput.
- End-to-end market data to paper fill latency.

## Riesgos Tecnicos

- Unrealistic paper fills can invalidate strategy confidence.
- Weak separation can create accidental live exposure.

## Dependencias

- OMS/execution boundary.
- Risk engine.
- Market data gateway.

## Resultado Esperado

A controlled paper-trading mode that can support strategy validation before live trading.
