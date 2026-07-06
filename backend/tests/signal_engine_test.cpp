#include "signal/signal_engine.hpp"

#include "core/fixed_point.hpp"
#include "engine/order_book.hpp"
#include "persist/event_journal.hpp"
#include "risk/risk_manager.hpp"
#include "trading/order_manager.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>

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
constexpr const char* kJournalPath = "data/test_signal_engine_events.jsonl";

signal::SignalCandidate make_candidate(uint8_t side, double price, double quantity) {
    signal::SignalCandidate candidate{};
    candidate.strategy_id = "test_strategy";
    candidate.model_version = "test_model_v1";
    candidate.feature_snapshot_id = "snapshot_1";
    candidate.symbol = kSymbol;
    candidate.side = side;
    candidate.price = price;
    candidate.quantity = quantity;
    candidate.timestamp_ns = 1'700'000'000'000'000'000ULL;

    // gross = 0.65*40 + 0.35*(-15) = 20.75; cost = 4 + 1 + 0.5 + 0.3 = 5.8; net = 14.95
    candidate.ev_inputs.p_win = 0.65;
    candidate.ev_inputs.avg_win_bps = 40.0;
    candidate.ev_inputs.avg_loss_bps = -15.0;
    candidate.ev_inputs.confidence = 0.8;
    candidate.ev_inputs.notional = price * quantity;
    candidate.ev_inputs.costs.taker_fee_bps = 2.0;
    candidate.ev_inputs.costs.half_spread_bps = 0.5;
    candidate.ev_inputs.costs.slippage_bps = 0.5;
    candidate.ev_inputs.costs.market_impact_bps = 0.3;
    return candidate;
}

void seed_maker(engine::OrderBook& book, uint64_t order_id, uint8_t side, double price, double quantity) {
    Order maker{};
    maker.order_id = order_id;
    maker.timestamp_ns = 1'700'000'000'000'000'000ULL;
    maker.price = price;
    maker.quantity = quantity;
    core::normalize_order_scalars(&maker);
    std::strncpy(maker.symbol, kSymbol, sizeof(maker.symbol) - 1);
    maker.side = side;
    maker.type = ORDER_TYPE_LIMIT;
    maker.tif = TIF_GTC;
    CHECK(book.add_order(maker));
}

bool journal_contains(const std::string& needle_a, const std::string& needle_b) {
    std::ifstream in(kJournalPath);
    if (!in.is_open()) return false;
    std::string line;
    while (std::getline(in, line)) {
        if (line.find(needle_a) != std::string::npos &&
            line.find(needle_b) != std::string::npos) {
            return true;
        }
    }
    return false;
}

} // namespace

int main() {
    std::remove(kJournalPath);

    auto book = std::make_shared<engine::OrderBook>(kSymbol);
    auto risk = std::make_shared<risk::RiskManager>(risk::RiskLimits{
        1'000'000.0,
        1'000'000.0,
        1'000'000.0
    });
    auto journal = std::make_shared<persist::EventJournal>(kJournalPath);
    auto oms = std::make_shared<trading::OrderManager>(risk, book, journal);

    signal::SignalEngine::Config config{};
    config.gate.min_ev_bps_threshold = 3.0;
    config.gate.min_confidence = 0.55;
    config.gate.observation_size_haircut = 0.25;
    config.first_signal_id = 1;
    config.first_order_id = 5'000;
    signal::SignalEngine engine(config, oms);

    // 1) Accepted candidate crosses seeded liquidity, fills, and is journaled
    //    with the originating signal id.
    seed_maker(*book, 900'001, SIDE_SELL, 100.0, 1.0);
    const signal::Signal accepted = engine.process(make_candidate(SIDE_BUY, 100.0, 1.0));
    CHECK(accepted.signal_id == 1);
    CHECK(accepted.ev_result.accepted);
    CHECK(accepted.ev_result.reject_reason == ev::RejectReason::None);
    CHECK_NEAR(accepted.ev_result.ev_net_bps, 14.95, 1e-9);
    CHECK(accepted.submitted_order_id == 5'000);
    CHECK(accepted.order_accepted);
    CHECK_NEAR(accepted.submitted_quantity, 1.0, 1e-12);

    trading::OrderState state{};
    CHECK(oms->get_order_state(5'000, &state));
    CHECK(state.status == trading::OrderStatus::Filled);
    CHECK_NEAR(risk->net_position_for_symbol(kSymbol), 1.0, 1e-9);

    journal->flush();
    CHECK(journal_contains("\"order_id\":5000", "\"related_signal_id\":1"));
    CHECK(journal_contains("\"type\":\"trade_executed\"", "\"related_signal_id\":1"));

    // 2) Negative-EV candidate is rejected by the gate and never reaches OMS.
    signal::SignalCandidate weak = make_candidate(SIDE_BUY, 100.0, 1.0);
    weak.ev_inputs.p_win = 0.50;
    weak.ev_inputs.avg_win_bps = 10.0;
    weak.ev_inputs.avg_loss_bps = -10.0;  // gross 0, net negative
    const signal::Signal rejected = engine.process(weak);
    CHECK(!rejected.ev_result.accepted);
    CHECK(rejected.ev_result.reject_reason == ev::RejectReason::EvBelowThreshold);
    CHECK(rejected.submitted_order_id == 0);
    CHECK(!rejected.order_accepted);

    // 3) Quarantined strategy is blocked regardless of EV quality.
    engine.set_strategy_state("test_strategy", ev::StrategyState::Quarantined);
    const signal::Signal blocked = engine.process(make_candidate(SIDE_BUY, 100.0, 1.0));
    CHECK(!blocked.ev_result.accepted);
    CHECK(blocked.ev_result.reject_reason == ev::RejectReason::LifecycleBlocked);
    CHECK(blocked.submitted_order_id == 0);

    // 4) UnderObservation applies the 25% size haircut on the submitted order.
    engine.set_strategy_state("test_strategy", ev::StrategyState::UnderObservation);
    seed_maker(*book, 900'002, SIDE_SELL, 100.0, 1.0);
    const signal::Signal observed = engine.process(make_candidate(SIDE_BUY, 100.0, 1.0));
    CHECK(observed.ev_result.accepted);
    CHECK_NEAR(observed.ev_result.size_multiplier, 0.25, 1e-12);
    CHECK_NEAR(observed.submitted_quantity, 0.25, 1e-12);
    CHECK(observed.submitted_order_id == 5'001);
    CHECK(observed.order_accepted);

    trading::OrderState observed_state{};
    CHECK(oms->get_order_state(5'001, &observed_state));
    CHECK(observed_state.initial_lots == core::to_quantity_lots(0.25));

    // 5) Funnel stats add up.
    const signal::SignalEngineStats stats = engine.stats();
    CHECK(stats.candidates == 4);
    CHECK(stats.gate_accepted == 2);
    CHECK(stats.orders_submitted == 2);
    CHECK(stats.orders_accepted == 2);
    CHECK(stats.rejects_by_reason[static_cast<size_t>(ev::RejectReason::EvBelowThreshold)] == 1);
    CHECK(stats.rejects_by_reason[static_cast<size_t>(ev::RejectReason::LifecycleBlocked)] == 1);

    const signal::GateLatencySnapshot latency = engine.gate_latency_snapshot();
    CHECK(latency.samples == 4);

    if (g_failures != 0) {
        std::printf("signal_engine_test FAILED (%d checks)\n", g_failures);
        return 1;
    }
    std::printf("signal_engine_test passed\n");
    return 0;
}
