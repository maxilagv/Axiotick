// Block 1 end-to-end harness: SMA-crossover strategy -> SignalEngine (EV gate)
// -> OrderManager -> RiskManager/OrderBook, with the full explainability audit
// trail in audit.log and order/signal correlation in the event journal.
//
// The strategy below is a deliberately trivial demo (SMA cross + volatility
// heuristics for p_win / expected moves). It exists to exercise the decision
// machinery, not as an alpha claim.

#include "analysis/sma_crossover_strategy.hpp"
#include "audit/logger.hpp"
#include "core/fixed_point.hpp"
#include "core/pipeline_telemetry.hpp"
#include "core/time_utils.hpp"
#include "core/trace_context.hpp"
#include "core/types.h"
#include "engine/order_book.hpp"
#include "ev/ev_gate.hpp"
#include "persist/event_journal.hpp"
#include "risk/risk_manager.hpp"
#include "signal/signal_engine.hpp"
#include "trading/order_manager.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace {

constexpr const char* kSymbol = "BTC/USDT";
constexpr const char* kJournalPath = "data/signal_demo_events.jsonl";
constexpr double kTargetNotional = 2'000.0;

// Deterministic linear congruential generator so every run is reproducible.
class Lcg {
public:
    explicit Lcg(uint64_t seed) : state_(seed) {}

    double next_unit() {
        state_ = state_ * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<double>((state_ >> 11) & ((1ULL << 53) - 1)) /
               static_cast<double>(1ULL << 53);
    }

    double next_gaussian() {
        // Box-Muller on two uniforms.
        const double u1 = std::max(1e-12, next_unit());
        const double u2 = next_unit();
        return std::sqrt(-2.0 * std::log(u1)) * std::cos(6.283185307179586 * u2);
    }

private:
    uint64_t state_;
};

std::vector<MarketTick> synthesize_ticks(size_t count) {
    std::vector<MarketTick> ticks;
    ticks.reserve(count);

    Lcg rng(20260705ULL);
    double price = 50'000.0;
    uint64_t ts = 1'700'000'000'000'000'000ULL;

    for (size_t i = 0; i < count; ++i) {
        // Three phases: uptrend, choppy range, downtrend.
        double drift_bps = 0.0;
        double vol_bps = 5.0;
        if (i < count / 3) {
            drift_bps = 0.8;
        } else if (i < (2 * count) / 3) {
            drift_bps = 0.0;
            vol_bps = 2.0;
        } else {
            drift_bps = -0.8;
            vol_bps = 6.0;
        }

        const double ret_bps = drift_bps + vol_bps * rng.next_gaussian();
        price *= (1.0 + ret_bps / 10'000.0);
        ts += 100'000'000ULL;  // 100ms per tick

        MarketTick tick{};
        tick.timestamp_ns = ts;
        tick.price = price;
        tick.quantity = 0.05 + 0.10 * rng.next_unit();
        std::strncpy(tick.symbol, kSymbol, sizeof(tick.symbol) - 1);
        std::strncpy(tick.source, "SYNTH", sizeof(tick.source) - 1);
        tick.side = static_cast<uint8_t>((rng.next_unit() < 0.5) ? SIDE_BUY : SIDE_SELL);
        // Synthetic source doubles as ingress: this process mints the tick.
        tick.ingress_ns = argentum::core::wall_now_ns();
        ticks.push_back(tick);
    }

    return ticks;
}


} // namespace

int main() {
    using namespace argentum;

    std::remove(kJournalPath);

    auto book = std::make_shared<engine::OrderBook>(kSymbol);
    auto risk = std::make_shared<risk::RiskManager>(risk::RiskLimits{
        100'000.0,   // max_order_value
        500'000.0,   // max_position_exposure
        25.0         // max_daily_loss: small on purpose so the auto kill switch can fire
    });
    auto journal = std::make_shared<persist::EventJournal>(kJournalPath);
    auto oms = std::make_shared<trading::OrderManager>(risk, book, journal);

    signal::SignalEngine::Config engine_config{};
    engine_config.gate.min_ev_bps_threshold = 3.0;
    engine_config.gate.min_confidence = 0.55;
    engine_config.gate.observation_size_haircut = 0.25;
    signal::SignalEngine engine(engine_config, oms);

    analysis::SmaCrossoverStrategy strategy(kSymbol, kTargetNotional);
    const std::vector<MarketTick> ticks = synthesize_ticks(3'000);

    std::printf("=== Axiotick signal demo (Block 1) ===\n");
    std::printf("ticks=%zu symbol=%s journal=%s\n\n", ticks.size(), kSymbol, kJournalPath);

    uint64_t maker_id = 900'000'000ULL;
    size_t fills = 0;
    core::PipelineTelemetry telemetry;
    core::TraceSpans spans;

    for (const MarketTick& tick : ticks) {
        spans.reset();
        spans.tick_id = core::next_tick_id();
        spans.ingress_wall_ns = tick.ingress_ns;
        spans.stamp(core::TraceStage::TickIngress);

        risk->maybe_roll_day(tick.timestamp_ns);
        risk->mark_to_market(kSymbol, tick.price);

        auto candidate = strategy.on_tick(tick);
        if (!candidate) {
            continue;
        }

        // Seed synthetic counterparty liquidity directly into the book (it is
        // not our risk), sized to fully absorb the candidate order.
        Order maker{};
        maker.order_id = ++maker_id;
        maker.timestamp_ns = tick.timestamp_ns;
        maker.price = candidate->price;
        maker.quantity = candidate->quantity;
        core::normalize_order_scalars(&maker);
        std::strncpy(maker.symbol, kSymbol, sizeof(maker.symbol) - 1);
        maker.side = static_cast<uint8_t>((candidate->side == SIDE_BUY) ? SIDE_SELL : SIDE_BUY);
        maker.type = ORDER_TYPE_LIMIT;
        maker.tif = TIF_GTC;
        (void)book->add_order(maker);

        const signal::Signal signal = engine.process(*candidate, &spans);
        telemetry.absorb(spans);
        if (signal.order_accepted) {
            ++fills;
        }

        // Remove leftover synthetic liquidity so it cannot interact with the
        // next signal at a stale price.
        (void)book->cancel_order(maker.order_id);
    }

    const signal::SignalEngineStats stats = engine.stats();
    const signal::GateLatencySnapshot gate_latency = engine.gate_latency_snapshot();

    std::printf("--- signal funnel ---\n");
    std::printf("candidates          : %llu\n", static_cast<unsigned long long>(stats.candidates));
    std::printf("gate_accepted       : %llu\n", static_cast<unsigned long long>(stats.gate_accepted));
    std::printf("orders_submitted    : %llu\n", static_cast<unsigned long long>(stats.orders_submitted));
    std::printf("orders_accepted     : %llu (fills=%zu)\n",
                static_cast<unsigned long long>(stats.orders_accepted), fills);
    for (size_t i = 0; i < stats.rejects_by_reason.size(); ++i) {
        if (stats.rejects_by_reason[i] == 0) continue;
        std::printf("rejected[%-22s]: %llu\n",
                    ev::to_string(static_cast<ev::RejectReason>(i)),
                    static_cast<unsigned long long>(stats.rejects_by_reason[i]));
    }

    std::printf("\n--- risk state ---\n");
    std::printf("net_position(%s) : %.6f\n", kSymbol, risk->net_position_for_symbol(kSymbol));
    std::printf("realized_pnl       : %.2f\n", risk->realized_pnl());
    std::printf("unrealized_pnl     : %.2f\n", risk->unrealized_pnl());
    std::printf("daily_pnl          : %.2f (limit=25.00)\n", risk->daily_pnl());
    std::printf("kill_switch        : %s\n", risk->kill_switch_active() ? "ACTIVE" : "inactive");

    std::printf("\n--- EV gate latency (per evaluate call) ---\n");
    std::printf("samples=%llu p50=%lluns p95=%lluns p99=%lluns p99.9=%lluns max=%lluns\n",
                static_cast<unsigned long long>(gate_latency.samples),
                static_cast<unsigned long long>(gate_latency.p50_ns),
                static_cast<unsigned long long>(gate_latency.p95_ns),
                static_cast<unsigned long long>(gate_latency.p99_ns),
                static_cast<unsigned long long>(gate_latency.p999_ns),
                static_cast<unsigned long long>(gate_latency.max_ns));

    std::printf("\n");
    telemetry.print(stdout);
    const char* kLatencyJsonPath = "data/signal_demo_latency.json";
    if (telemetry.write_json(kLatencyJsonPath, "signal_demo")) {
        std::printf("latency json: %s\n", kLatencyJsonPath);
    }

    // Operator drill: trigger the kill switch manually, show that the OMS path
    // rejects while it is active, then reset it.
    std::printf("\n--- kill switch drill ---\n");
    const bool was_active = risk->kill_switch_active();
    if (!was_active) {
        risk->trigger_kill_switch("demo_manual_drill");
    }

    Order drill{};
    drill.order_id = 999'999'999ULL;
    drill.timestamp_ns = ticks.back().timestamp_ns;
    drill.price = ticks.back().price;
    drill.quantity = 0.01;
    std::strncpy(drill.symbol, kSymbol, sizeof(drill.symbol) - 1);
    drill.side = SIDE_BUY;
    drill.type = ORDER_TYPE_LIMIT;
    drill.tif = TIF_GTC;
    const trading::OrderSubmissionResult drill_result = oms->submit_order(drill);
    std::printf("order under kill switch: accepted=%s (expected false, reason=risk_rejected)\n",
                drill_result.accepted ? "true" : "false");

    if (!was_active) {
        risk->reset_kill_switch("demo_drill_complete");
        std::printf("kill switch reset      : active=%s\n",
                    risk->kill_switch_active() ? "true" : "false");
    } else {
        std::printf("kill switch stays ACTIVE (tripped by daily loss during the run; manual reset only)\n");
    }

    journal->flush();
    std::printf("\naudit trail: audit.log (signal_decision events) | journal: %s\n", kJournalPath);
    return drill_result.accepted ? 1 : 0;
}
