# Differentiation Thesis: What Axiotick Attacks That Other Trading Stacks Do Not

**Status: target-state research document.** Everything in this file is a capability roadmap, not implemented behavior, except where a section explicitly says it builds on shipped components. It follows the repository's honesty rule: implemented, partial, or target — never "done" when it is not.

Date: July 2026. Origin: internal research pass over the decision engine (Blocks 1-3) asking one question — *given that Axiotick cannot and should not win the raw-speed race, where is the war it can actually win?*

---

## The framing decision

There are two definitions of "perfect latency", and almost every trading operation chases the wrong one for its size:

1. **Raw speed** — being first in the queue. This only pays for colocated HFT market making. It requires FPGAs, microwave links and exchange colocation, and it is explicitly out of scope per [`LATENCY_AND_SCALE_TARGETS.md`](LATENCY_AND_SCALE_TARGETS.md).
2. **Deterministic, economically accounted latency** — knowing exactly how long every decision takes wire-to-wire, guaranteeing the tail (p99.9) never explodes, and — the part nobody does — knowing **how many bps each millisecond costs** and feeding that cost into the EV-Gate.

Axiotick's own baseline numbers force this framing: the EV-Gate decides in ~100 ns and the regime classifier in ~600 ns — five orders of magnitude under the 25 ms decision budget. The decision math is already "fast enough forever". The real war is in the data paths, the measurement discipline, and the *economic use* of latency knowledge.

**Thesis: Axiotick's edge is not being the fastest. It is being the only engine that knows exactly what its speed is worth in bps, per strategy and per venue, and refuses every trade where that arithmetic does not close — with an audit trail proving it.**

Raw speed is owned by dozens of firms. Economically accounted, explainable, per-decision-audited latency is owned by almost nobody outside the top tier — and they do not sell it.

---

## Capability map

Seven capability areas, each attacking something the rest of the market does not. Ordered A-G by theme; prioritization is at the end.

### A. The latency → cost → EV loop (strongest differentiator)

Today the EV-Gate consumes `slippage_bps` and `market_impact_bps` as static inputs. Retail stacks ignore these costs; mid-size funds run transaction cost analysis (TCA) as an offline quarterly report. **Nobody closes the loop at runtime.**

Missing pieces:

- **Post-fill markouts.** For every fill, measure price drift at +100 ms, +1 s, +10 s, +60 s. This measures realized *adverse selection* per venue and per strategy — how much the market moved against us immediately after execution. It is the metric that separates "my signal was wrong" from "I was late".
- **Cost-of-latency curve per strategy.** Regress markout against the engine's own decision latency for that trade. Output: "in this strategy, every extra 10 ms costs 0.8 bps". This tells us *where* optimization spend pays, with an economic criterion instead of HFT folklore.
- **Feedback into the EV-Gate.** `cost_bps += adverse_selection_bps(current_latency, venue)`, continuously calibrated from our own fills. The gate evolves from rejecting expensive signals to rejecting signals *we specifically cannot capture at our speed*.

Builds on: `ExecutionQualityTracker` (fill ratio, slippage, per-venue latency), the EV-Gate cost model, the event journal.

### B. Micro-regime: sub-second analysis done right

The `RegimeClassifier` operates on bars arriving every seconds-to-minutes. The gap is **regime at tick scale**:

- **Order Flow Imbalance (OFI)** and book pressure, updated O(1) per tick over the L2 depth model that routing v2 already has. The best-documented short-horizon predictor in market microstructure.
- **Flow toxicity** (VPIN family): detect when incoming flow is informed — i.e. when we are the dumb side of the trade. A `Toxic` micro-regime tightens the EV-Gate threshold exactly the way `Stress` does today.
- **Liquidation-cascade and sweep detection** (critical in crypto): aggressor sequences sweeping levels → millisecond-to-second blackout signal.
- **Cross-venue lead-lag.** If venue A stably leads venue B by N ms, that is simultaneously a signal and a router input. With the 8-15 venue target, the lead-lag matrix is free differentiation.

Architectural fit: a `MicroRegimeClassifier` sibling of the existing classifier — same pure, no-I/O pattern, incremental O(1) features, conditioning the same `regime_multiplier`.

### C. Clock discipline and wire-to-wire measurement

Without this, "analyzing every fraction of a second" is unverifiable fiction. Today the engine measures components (gate, classifier) but not the full wire-to-wire path.

- **Disciplined clocks** (PTP where possible, well-configured NTP minimum) and timestamps at every boundary: packet arrival, parse complete, bus publish, decision, order send, venue ack. Without clock discipline, a +100 ms markout is noise.
- **A trace context traversing the pipeline.** Orders are already journaled with the originating signal id — extend that to a per-stage timestamp chain, and deterministic replay yields per-stage latency histograms for free.
- **SLOs on p99.9 and max, not just p50/p99.** Tail jitter kills precisely during stress — when `Stress`-regime signals are worth the most. The measurement doctrine already demands p99.9; it must become an acceptance threshold per module (the "Current Measurement Gap" in `LATENCY_AND_SCALE_TARGETS.md`).

### D. Scheduled-event risk gate

Cheap to build, disproportionate differentiation: a machine-readable economic calendar (FOMC, CPI, NFP, halvings, option expiries, listings) that automatically widens EV-Gate thresholds or imposes blackout windows ±N minutes. Most retail bots die on scheduled news — which is by definition *predictable in time even if not in direction*. The regime classifier detects stress *after it happens*; this gate anticipates it by calendar. Fits as one more gate condition with its own auditable rejection reason (`scheduled_event_blackout`).

### E. Predictive engine without breaking the golden rule

The rule stands: Python never in the hot path. The path to real per-tick prediction without violating it:

- **Compiled inference in C++**: hand-rolled linear/logistic models (sub-µs), compiled GBMs (treelite/lleaves), or ONNX Runtime with pre-warmed sessions for small nets. Budget: < 10 µs per inference. Train in Python offline, export versioned, execute in C++ — exactly the contract the planned model registry describes.
- **Shadow trading / champion-challenger**: the candidate model runs in parallel against production receiving the same events; its decisions are journaled but not executed; promotion is a statistical test on the divergence, not an act of faith. Natural extension of strategy lifecycle governance — which already has the states and the detectors — applied to models.
- **Incremental O(1) features**: every online feature updates per tick without window recomputation (Welford is already used in Page-Hinkley; the same principle generalizes). The planned online feature store should be this: a shared-memory struct updated incrementally, not a service call.

### F. Self-surveillance and deterministic resilience

What regulators will demand and retail stacks do not have:

- **Self-surveillance**: detect our own anomalous behavior — runaway orders, accidental self-trading between strategies, pathological cancellation rates. The kill switch pointed inward. The event journal already contains ~80% of the raw material.
- **Cancel-on-disconnect / dead-man switch** with the venue: if the process dies or loses network, resting orders must cancel themselves. Hard prerequisite in the live trading readiness checklist.
- **Continuous reconciliation** of local position vs venue-reported position, with drift alarms. A perfect internal state is worthless if it has diverged from reality.
- **HA via state-machine replication** (Aeron-Cluster style): the deterministic replay journal is exactly the foundation — the same journal that rebuilds state offline can replicate state to a hot standby, giving failover without sacrificing the determinism that is Axiotick's identity.

### G. Hot-path infrastructure (when the economics justify it)

To bring the data path from milliseconds to tens of microseconds — which the README itself identifies as the real bottleneck:

- **Linux for production runtime** (development on Windows stays fine): core pinning, `isolcpus`, busy-polling, huge pages, NUMA-awareness.
- **SIMD parsing** (simdjson for crypto websockets; SBE/binary FIX where the venue offers it) and redundant connections to the same feed, taking first arrival.
- **Single-writer pattern** (LMAX Disruptor style) per instrument shard — compatible with the already-planned OMS sharding.
- **Kernel bypass (DPDK/Onload) only if the cost-of-latency curve from capability A proves it pays.** That is the point: expensive infrastructure gets justified with our own data, not with HFT folklore.

---

## Prioritization

Ordered by (differentiation ÷ build cost), each stage standing on something already shipped:

| # | Block | Capability | Why this order |
|---|---|---|---|
| 1 | **Wire-to-wire measurement + clock discipline** | C | Everything else is unverifiable without it; already an acknowledged measurement gap |
| 2 | Markouts + cost-of-latency curve + EV-Gate feedback | A | The piece nobody else has; most aligned with the product thesis |
| 3 | Scheduled-event risk gate | D | Weeks of work, disproportionate differentiation |
| 4 | Micro-regime (OFI / toxicity / sweeps) | B | The real materialization of sub-second analysis |
| 5 | Compiled inference + shadow trading | E | The predictive engine, with fail-closed safety already waiting for it |
| 6 | Self-surveillance + cancel-on-disconnect + reconciliation | F | Hard prerequisite for live trading |
| 7 | Linux hot-path tuning / SIMD parsing | G | Last, and only where the curve from block 2 proves it pays |

## What Axiotick deliberately does NOT chase

- HFT colocation and the nanosecond race — capital-intensive, winner-take-all, off-thesis.
- More signal features before the TCA loop exists — un-costed alpha is indistinguishable from noise.
- Any capability that cannot emit an explainable, journaled, replayable decision record. Explainability is not a tax on the differentiators above; it is one of them.

## Relationship to existing roadmap

This document does not replace the phase roadmap ([`roadmap/README.md`](roadmap/README.md)); it re-prioritizes *within* it. Block 1 (capability C) lands inside Fase 01 (core y latencia) and Fase 10 (observabilidad). Capability A extends Fase 06/07 (execution quality, backtesting). Capability E is the concrete shape of Fase 09 (AI). Capability F feeds Fase 11 (seguridad) and the live-readiness checklist.

Each block gets its own plan document and, where architecture is decided, its own ADR — starting with Block 1: [`PLAN_WIRE_TO_WIRE_LATENCY.md`](PLAN_WIRE_TO_WIRE_LATENCY.md).
