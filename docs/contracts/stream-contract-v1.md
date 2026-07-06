# Stream Contract v1

This document defines the canonical event payloads produced by the market gateway.

## Tick Event

```json
{
  "event": "tick",
  "symbol": "BTC/USDT",
  "timestamp_ns": 1700000001111111111,
  "price": 50123.45,
  "quantity": 0.25,
  "side": "buy",
  "source": "BINANCE"
}
```

Required fields:
- `event`: must be `"tick"`.
- `symbol`: normalized symbol in `AAA/BBB` form.
- `timestamp_ns`: unix nanoseconds.
- `price`: positive decimal.
- `quantity`: positive decimal.
- `side`: `"buy"` or `"sell"`.
- `source`: feed source identifier.

## Order Ack Event

```json
{
  "event": "order_ack",
  "order_id": 9001,
  "accepted": true,
  "resting": true,
  "filled_quantity": 0.0,
  "remaining_quantity": 1.0,
  "reject_reason": "none",
  "gateway_reject_reason": "none"
}
```

`gateway_reject_reason` values:
- `none`
- `unauthorized`
- `rate_limited`

`reject_reason` values:
- `none`
- `invalid_order`
- `duplicate_order_id`
- `risk_rejected`
- `internal_error`
- `liquidity_unavailable`

## Health Payload

```json
{
  "status": "ok",
  "timestamp_ns": 1700000002222222222,
  "ticks_received": 12,
  "ticks_decoded": 12,
  "decode_errors": 0,
  "order_requests": 4,
  "order_accepted": 2,
  "order_rejected": 2,
  "auth_failures": 1,
  "rate_limited": 1,
  "tracked_symbols": 1
}
```
