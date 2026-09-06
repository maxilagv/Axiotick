// End-to-end trace correlation through the OMS and journal (ADR 0014):
//   1. an order submitted with a signal id + trace context journals
//      related_signal_id AND tick_id on accept;
//   2. cancel and modify events KEEP both ids (regression: they used to
//      journal related_signal_id = 0);
//   3. pre-Block-1 journal lines (without tick_id) still replay — backward
//      compatibility of the JSONL schema.

#include "core/trace_context.hpp"
#include "engine/order_book.hpp"
#include "persist/event_journal.hpp"
#include "risk/risk_manager.hpp"
#include "trading/order_manager.hpp"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

// CHECK (not assert): assert is compiled out in Release, and CTest runs Release.
#define CHECK(cond) do { if (!(cond)) { std::cerr << "CHECK failed: " << #cond << " line=" << __LINE__ << std::endl; return 1; } } while (0)

namespace {

constexpr const char* kJournalPath = "data/trace_correlation_test_events.jsonl";
constexpr const char* kLegacyJournalPath = "data/trace_correlation_test_legacy.jsonl";
constexpr uint64_t kSignalId = 7777;

Order make_gtc_limit(uint64_t order_id, double price, double quantity) {
    Order order{};
    order.order_id = order_id;
    order.price = price;
    order.quantity = quantity;
    order.side = SIDE_BUY;
    order.type = ORDER_TYPE_LIMIT;
    order.tif = TIF_GTC;
    std::strncpy(order.symbol, "EUR/USD", sizeof(order.symbol) - 1);
    return order;
}

std::vector<std::string> read_lines(const std::string& path) {
    std::vector<std::string> lines;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty()) lines.push_back(line);
    }
    return lines;
}

bool line_has(const std::string& line, const std::string& needle) {
    return line.find(needle) != std::string::npos;
}

} // namespace

int main() {
    std::remove(kJournalPath);
    std::remove(kLegacyJournalPath);

    argentum::core::TraceSpans spans;
    spans.tick_id = 424242;
    spans.stamp(argentum::core::TraceStage::TickIngress);

    const std::string tick_marker = "\"tick_id\":424242";
    const std::string signal_marker = "\"related_signal_id\":7777";

    {
        auto book = std::make_shared<argentum::engine::OrderBook>("EUR/USD");
        auto risk = std::make_shared<argentum::risk::RiskManager>(argentum::risk::RiskLimits{
            1'000'000.0, 10'000'000.0, 0.0, {}});
        auto journal = std::make_shared<argentum::persist::EventJournal>(kJournalPath);
        argentum::trading::OrderManager oms(risk, book, journal);

        // 1) Submit: rests in the empty book (no liquidity), then cancel.
        const auto submit = oms.submit_order(make_gtc_limit(1, 100.0, 5.0), kSignalId, &spans);
        CHECK(submit.accepted);
        CHECK(submit.resting);
#ifdef ARGENTUM_ENABLE_LATENCY_TRACE
        CHECK(spans.stamped(argentum::core::TraceStage::RiskVerdict));
        CHECK(spans.stamped(argentum::core::TraceStage::OmsAccept));
        CHECK(spans.stamped(argentum::core::TraceStage::JournalEnqueue));
        CHECK(spans.at(argentum::core::TraceStage::OmsAccept) >=
               spans.at(argentum::core::TraceStage::RiskVerdict));
#endif
        CHECK(oms.cancel_order(1));

        // 2) Submit another and modify it (replace path).
        const auto submit2 = oms.submit_order(make_gtc_limit(2, 101.0, 5.0), kSignalId, &spans);
        CHECK(submit2.accepted);
        CHECK(oms.modify_order(2, 102.0, 4.0));

        journal->flush();
    }

    const std::vector<std::string> lines = read_lines(kJournalPath);
    CHECK(lines.size() >= 4);  // accept(1), cancel(1), accept(2), replace(2)

    bool accept_ok = false;
    bool cancel_ok = false;
    bool replace_ok = false;
    for (const std::string& line : lines) {
        if (line_has(line, "\"type\":\"order_accepted\"") && line_has(line, "\"order_id\":1")) {
            accept_ok = line_has(line, signal_marker) && line_has(line, tick_marker);
        }
        if (line_has(line, "\"type\":\"order_canceled\"") && line_has(line, "\"order_id\":1")) {
            // THE regression this test exists for: cancels used to journal 0.
            cancel_ok = line_has(line, signal_marker) && line_has(line, tick_marker);
        }
        if (line_has(line, "\"type\":\"order_replaced\"") && line_has(line, "\"order_id\":2")) {
            replace_ok = line_has(line, signal_marker) && line_has(line, tick_marker);
        }
    }
    CHECK(accept_ok);
    CHECK(cancel_ok);
    CHECK(replace_ok);

    // Replay of the new-format journal reconstructs cleanly.
    {
        argentum::persist::ReplaySummary summary{};
        CHECK(argentum::persist::EventReplayer::replay_file(kJournalPath, &summary));
        CHECK(summary.total_events == lines.size());
        CHECK(summary.accepted == 2);
        CHECK(summary.canceled == 1);
        CHECK(summary.replaced == 1);
    }

    // 3) Backward compat: a pre-Block-1 line (no tick_id field) still replays.
    {
        std::ofstream legacy(kLegacyJournalPath, std::ios::out | std::ios::trunc);
        legacy << "{\"seq\":1,\"timestamp_ns\":1700000000000000001,\"type\":\"order_accepted\","
                  "\"order_id\":9,\"related_order_id\":0,\"related_signal_id\":5,"
                  "\"price_ticks\":1000000,\"quantity_lots\":500,\"remaining_lots\":500,"
                  "\"reason_code\":0,\"side\":1,\"order_type\":2,\"tif\":1,\"resting\":true}\n";
        legacy << "{\"seq\":2,\"timestamp_ns\":1700000000000000002,\"type\":\"order_canceled\","
                  "\"order_id\":9,\"related_order_id\":0,\"related_signal_id\":5,"
                  "\"price_ticks\":1000000,\"quantity_lots\":500,\"remaining_lots\":0,"
                  "\"reason_code\":0,\"side\":1,\"order_type\":2,\"tif\":1,\"resting\":false}\n";
        legacy.close();

        argentum::persist::ReplaySummary summary{};
        CHECK(argentum::persist::EventReplayer::replay_file(kLegacyJournalPath, &summary));
        CHECK(summary.total_events == 2);
        CHECK(summary.accepted == 1);
        CHECK(summary.canceled == 1);
        CHECK(summary.monotonic_seq);
    }

    std::printf("trace_correlation_journal_test: OK\n");
    return 0;
}
