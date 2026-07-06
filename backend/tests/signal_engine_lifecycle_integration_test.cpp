#include "signal/signal_engine.hpp"

#include "core/fixed_point.hpp"
#include "engine/order_book.hpp"
#include "persist/event_journal.hpp"
#include "risk/risk_manager.hpp"
#include "trading/order_manager.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>

static int g_failures = 0;
#define CHECK(cond) \
    do { \
        if (!(cond)) { \
            std::printf("CHECK FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures; \
        } \
    } while (0)
#define CHECK_NEAR(a, b, eps) CHECK(std::abs((a) - (b)) <= (eps))

using namespace argentum;

namespace {

constexpr const char* kSymbol = "BTC/USDT";
constexpr const char* kJournalPath = "data/test_signal_lifecycle_events.jsonl";
constexpr uint64_t kSecond = 1'000'000'000ULL;

struct Harness {
    std::shared_ptr<engine::OrderBook> book;
    std::shared_ptr<risk::RiskManager> risk;
    std::shared_ptr<persist::EventJournal> journal;
    std::shared_ptr<trading::OrderManager> oms;
    std::unique_ptr<signal::SignalEngine> engine;
    uint64_t maker_id = 900'000'000ULL;

    explicit Harness(const signal::SignalEngine::Config& config) {
        std::remove(kJournalPath);
        book = std::make_shared<engine::OrderBook>(kSymbol);
        risk = std::make_shared<risk::RiskManager>(risk::RiskLimits{1e9, 1e9, 0.0});
        journal = std::make_shared<persist::EventJournal>(kJournalPath);
        oms = std::make_shared<trading::OrderManager>(risk, book, journal);
        engine = std::make_unique<signal::SignalEngine>(config, oms);
    }

    // Submits a strong-EV BUY candidate at `price` with a tiny horizon and a
    // fully-absorbing maker, so it always fills at exactly `price`.
    signal::Signal submit_buy(double price, uint64_t ts, double holding_hours) {
        Order maker{};
        maker.order_id = ++maker_id;
        maker.timestamp_ns = ts;
        maker.price = price;
        maker.quantity = 1.0;
        core::normalize_order_scalars(&maker);
        std::strncpy(maker.symbol, kSymbol, sizeof(maker.symbol) - 1);
        maker.side = SIDE_SELL;
        maker.type = ORDER_TYPE_LIMIT;
        maker.tif = TIF_GTC;
        CHECK(book->add_order(maker));

        signal::SignalCandidate candidate{};
        candidate.strategy_id = "integration_probe";
        candidate.model_version = "probe_v1";
        candidate.feature_snapshot_id = "probe";
        candidate.symbol = kSymbol;
        candidate.side = SIDE_BUY;
        candidate.price = price;
        candidate.quantity = 1.0;
        candidate.timestamp_ns = ts;
        candidate.ev_inputs.p_win = 0.65;
        candidate.ev_inputs.avg_win_bps = 40.0;
        candidate.ev_inputs.avg_loss_bps = -15.0;
        candidate.ev_inputs.confidence = 0.8;
        candidate.ev_inputs.notional = price * candidate.quantity;
        candidate.ev_inputs.costs.taker_fee_bps = 2.0;
        candidate.ev_inputs.costs.half_spread_bps = 0.5;
        candidate.ev_inputs.costs.slippage_bps = 0.5;
        candidate.ev_inputs.costs.market_impact_bps = 0.3;
        candidate.ev_inputs.costs.expected_holding_hours = holding_hours;

        const signal::Signal result = engine->process(candidate);
        (void)book->cancel_order(maker.order_id);
        return result;
    }
};

void test_fill_to_resolved_evaluation() {
    Harness harness{signal::SignalEngine::Config{}};
    const uint64_t t0 = 1'700'000'000ULL * kSecond;

    // Horizon 3.6s (0.001h).
    const signal::Signal accepted = harness.submit_buy(100.0, t0, 0.001);
    CHECK(accepted.ev_result.accepted);
    CHECK(accepted.order_accepted);
    CHECK(harness.engine->registry().health("integration_probe").pending_count == 1);

    // A tick before the horizon does not resolve.
    harness.engine->on_market_tick(kSymbol, 100.05, t0 + 1 * kSecond);
    CHECK(harness.engine->registry().health("integration_probe").pending_count == 1);

    // A tick past the horizon resolves against that tick's price.
    harness.engine->on_market_tick(kSymbol, 100.10, t0 + 5 * kSecond);
    const auto resolved = harness.engine->registry().resolved_history("integration_probe");
    CHECK(resolved.size() == 1);
    CHECK(resolved[0].signal_id == accepted.signal_id);
    CHECK_NEAR(resolved[0].realized_bps, 10.0, 1e-9);   // 100.00 -> 100.10
    CHECK_NEAR(resolved[0].predicted_ev_net_bps, 14.95, 1e-9);
    CHECK_NEAR(resolved[0].notional, 100.0, 1e-9);      // 1.0 @ 100
}

void test_manual_override_api_unchanged() {
    Harness harness{signal::SignalEngine::Config{}};
    const uint64_t t0 = 1'700'000'000ULL * kSecond;

    harness.engine->set_strategy_state("integration_probe", ev::StrategyState::Quarantined);
    CHECK(harness.engine->strategy_state("integration_probe") == ev::StrategyState::Quarantined);

    const signal::Signal blocked = harness.submit_buy(100.0, t0, 0.001);
    CHECK(!blocked.ev_result.accepted);
    CHECK(blocked.ev_result.reject_reason == ev::RejectReason::LifecycleBlocked);
    CHECK(blocked.submitted_order_id == 0);
}

void test_automatic_degradation_haircuts_next_candidate() {
    signal::SignalEngine::Config config{};
    config.registry.cusum_delta_bps = 0.5;
    config.registry.cusum_lambda_bps = 3.0;   // tight on purpose
    config.registry.sharpe_window = 1'000'000;
    config.registry.ev_decay_min_samples = 1'000'000;
    Harness harness{config};

    uint64_t ts = 1'700'000'000ULL * kSecond;

    // Two winners (+2bps), then one big loser (-10bps): with lambda=3 the
    // Page-Hinkley detector trips on the loser (PH = 7.5 > 3).
    const double resolutions[] = {100.02, 100.02, 99.90};
    for (double resolve_price : resolutions) {
        const signal::Signal accepted = harness.submit_buy(100.0, ts, 0.001);
        CHECK(accepted.order_accepted);
        ts += 5 * kSecond;
        harness.engine->on_market_tick(kSymbol, resolve_price, ts);
        ts += 1 * kSecond;
    }

    CHECK(harness.engine->strategy_state("integration_probe") ==
          ev::StrategyState::UnderObservation);
    const auto transitions =
        harness.engine->registry().transition_history("integration_probe");
    CHECK(transitions.size() == 1);
    CHECK(transitions[0].reason == "cusum_alarm");

    // The governance decision bites on the VERY NEXT candidate: accepted,
    // but at the under-observation size haircut.
    const signal::Signal observed = harness.submit_buy(100.0, ts, 0.001);
    CHECK(observed.ev_result.accepted);
    CHECK_NEAR(observed.ev_result.size_multiplier, 0.25, 1e-12);
    CHECK_NEAR(observed.submitted_quantity, 0.25, 1e-12);
}

} // namespace

int main() {
    test_fill_to_resolved_evaluation();
    test_manual_override_api_unchanged();
    test_automatic_degradation_haircuts_next_candidate();

    if (g_failures != 0) {
        std::printf("signal_engine_lifecycle_integration_test FAILED (%d checks)\n", g_failures);
        return 1;
    }
    std::printf("signal_engine_lifecycle_integration_test passed\n");
    return 0;
}
