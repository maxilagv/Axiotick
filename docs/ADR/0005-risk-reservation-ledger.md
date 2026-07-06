# ADR 0005: Risk Reservation Ledger by Order ID

## Context
The previous risk flow reserved and released exposure only from order/trade payload values.
When execution price differed from reserved price, committed exposure could drift.

## Decision
RiskManager now tracks reservations by `order_id`:
- reservation fields: side, reserved price ticks, remaining lots.
- `check_order`: creates reservation and updates committed exposure.
- `on_fill`: releases reserved exposure using reserved price/lots and books filled exposure using executed price.
- `on_cancel`: releases reservation using reserved price/remaining lots.

## Consequences
1. Committed exposure becomes deterministic and bounded by reservation state.
2. Fill price no longer corrupts reserved exposure accounting.
3. OMS/risk integration requires stable order ids and lot-accurate fill/cancel quantities.
