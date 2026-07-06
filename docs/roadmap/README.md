# Axiotick Roadmap

This roadmap converts Axiotick from a strong prototype into a serious quantitative trading platform over a one-year horizon. Each phase must be treated as an engineering gate, not a marketing milestone.

## Phase Index

- [Fase 00 - Auditoria](fase-00-auditoria/README.md)
- [Fase 01 - Core y Latencia](fase-01-core-y-latencia/README.md)
- [Fase 02 - Market Data](fase-02-market-data/README.md)
- [Fase 03 - Order Book](fase-03-order-book/README.md)
- [Fase 04 - Signal Engine](fase-04-signal-engine/README.md)
- [Fase 05 - Risk Engine](fase-05-risk-engine/README.md)
- [Fase 06 - OMS y Execution](fase-06-oms-y-execution/README.md)
- [Fase 07 - Backtesting](fase-07-backtesting/README.md)
- [Fase 08 - Paper Trading](fase-08-paper-trading/README.md)
- [Fase 09 - AI Research](fase-09-ai-research/README.md)
- [Fase 10 - Observabilidad](fase-10-observabilidad/README.md)
- [Fase 11 - Seguridad](fase-11-seguridad/README.md)
- [Fase 12 - Multi Exchange](fase-12-multi-exchange/README.md)
- [Fase 13 - HA y Produccion](fase-13-ha-y-produccion/README.md)
- [Fase 14 - Producto Financiable](fase-14-producto-financiable/README.md)

## Execution Strategy

The roadmap is intentionally ordered. Correctness, latency measurement, risk controls and replay come before broad connectivity or AI claims.

1. Establish repository truth and reproducibility.
2. Harden low-latency core and benchmark methodology.
3. Build market data, order book, signal, risk and OMS foundations.
4. Validate strategy behavior through backtesting and paper trading.
5. Add research, observability, security and multi-venue operations.
6. Prepare high-availability and finance-ready product packaging.

## Acceptance Rules

No phase is complete unless it has:

- Clear current-state documentation.
- Tests relevant to its risk profile.
- Benchmarks where latency or throughput matters.
- Explicit acceptance criteria.
- Known risks and dependencies.
- Evidence that mocks are isolated from production-intended paths.

## One-Year Milestones

- **30 days:** audit, reproducible build/tests/benchmarks, mock separation, latency metric definitions.
- **90 days:** hardened OMS/risk/replay, backtesting v1, paper-trading foundation.
- **180 days:** multi-source data in paper mode, signal engine, observability, security baseline.
- **365 days:** pilot-grade platform with controlled live readiness gates and finance-ready documentation.
