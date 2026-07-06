# Backend Structure

Axiotick is now organized as a backend-only repository. The structure separates executable entrypoints, implementation modules, public headers, benchmarks and tests.

## Layout

```text
backend/
  apps/
    node/          main runtime entrypoint
    replay/        replay CLI entrypoint
  benchmarks/      standalone benchmark executables
  include/         shared public headers used by modules and tests
  modules/
    api/           HTTP/WebSocket API gateway
    backtest/      historical replay/backtest implementation
    bus/           in-process event bus
    codec/         market/event codecs
    core/          shared low-level utilities
    datafeed/      parsers, normalizers and feed playback
    engine/        order book and matching
    gateway/       routing, FIX parsing and simulation primitives
    network/       socket primitives
    persist/       data writer and event journal
    risk/          risk checks and exposure accounting
    trading/       OMS/order lifecycle
  schema/          FlatBuffers and future schema definitions
  tests/           backend tests
```

## Rules

- `apps/` may compose modules, but business logic should live in `modules/`.
- `benchmarks/` must remain standalone and reproducible.
- `include/` remains the public compatibility layer until headers are moved into per-module include trees.
- `modules/*/src` contains implementation code for one domain.
- Tests should link against module libraries, not duplicate implementation code.
- New UI/frontend code is out of scope unless a later product decision reintroduces an operator console.

## Compatibility Note

Internal targets, namespaces and executable names still use the legacy `argentum_*` prefix. Product branding is Axiotick. A technical rename of targets, namespaces, metrics and generated schema should be handled in a separate compatibility-aware phase.
