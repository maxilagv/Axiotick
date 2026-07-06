#include "persist/event_journal.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#define CHECK(cond) do { if (!(cond)) { std::cerr << "CHECK failed: " << #cond << " line=" << __LINE__ << std::endl; return 1; } } while (0)

namespace {

argentum::persist::JournalEvent make_event(uint64_t order_id) {
    argentum::persist::JournalEvent event{};
    event.order_id = order_id;
    event.price_ticks = 100;
    event.quantity_lots = 10;
    event.remaining_lots = 10;
    event.side = SIDE_BUY;
    event.order_type = ORDER_TYPE_LIMIT;
    event.tif = TIF_GTC;
    event.type = argentum::persist::JournalEventType::OrderAccepted;
    return event;
}

}

int main() {
    {
        argentum::persist::LockFreeRingBuffer<argentum::persist::JournalEvent, 2> ring;
        CHECK(ring.try_push(make_event(1)));
        CHECK(ring.try_push(make_event(2)));
        CHECK(!ring.try_push(make_event(3)));
    }

    {
        constexpr size_t kTotal = 1'000'000;
        auto ring = std::make_unique<argentum::persist::LockFreeRingBuffer<argentum::persist::JournalEvent, 65536>>();
        std::atomic<size_t> consumed{0};

        std::thread consumer([&] {
            argentum::persist::JournalEvent event{};
            while (consumed.load(std::memory_order_acquire) < kTotal) {
                if (ring->try_pop(&event)) {
                    consumed.fetch_add(1, std::memory_order_release);
                }
            }
        });

        const auto start = std::chrono::steady_clock::now();
        for (size_t i = 0; i < kTotal; ++i) {
            auto event = make_event(static_cast<uint64_t>(i + 1));
            while (!ring->try_push(std::move(event))) {
                std::this_thread::yield();
            }
        }
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start);

        consumer.join();
        CHECK(consumed.load(std::memory_order_acquire) == kTotal);
        CHECK(elapsed.count() < 100);
    }

    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::string journal_path = "data/test_async_journal_" + std::to_string(nonce) + ".jsonl";
    const std::string gap_path = "data/test_async_journal_gap_" + std::to_string(nonce) + ".jsonl";
    std::error_code ec;
    std::filesystem::remove(journal_path, ec);
    std::filesystem::remove(gap_path, ec);

    {
        auto journal = std::make_unique<argentum::persist::EventJournal>(journal_path);
        for (uint64_t i = 1; i <= 2048; ++i) {
            CHECK(journal->append(make_event(i)));
        }

        const auto flush_start = std::chrono::steady_clock::now();
        journal->flush();
        const auto flush_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - flush_start);

        CHECK(journal->dropped_events() == 0);
        CHECK(journal->written_events() == 2048);
        CHECK(flush_elapsed.count() < 100);

        const auto latency = journal->latency_snapshot();
        CHECK(latency.samples > 0);
    }

    {
        std::ifstream in(journal_path);
        CHECK(in.is_open());

        std::string line;
        uint64_t last_seq = 0;
        size_t line_count = 0;
        while (std::getline(in, line)) {
            if (line.empty()) continue;
            ++line_count;

            const std::string marker = "\"seq\":";
            const size_t pos = line.find(marker);
            CHECK(pos != std::string::npos);
            const size_t begin = pos + marker.size();
            const uint64_t seq = static_cast<uint64_t>(std::strtoull(line.c_str() + begin, nullptr, 10));
            CHECK(seq == last_seq + 1);
            last_seq = seq;
        }

        CHECK(line_count == 2048);
    }

    {
        std::ofstream gap_out(gap_path, std::ios::out | std::ios::trunc);
        CHECK(gap_out.is_open());
        gap_out << "{\"seq\":1,\"timestamp_ns\":1,\"type\":\"order_accepted\",\"order_id\":1,\"related_order_id\":0,\"price_ticks\":1,\"quantity_lots\":1,\"remaining_lots\":1,\"reason_code\":0,\"side\":1,\"order_type\":2,\"tif\":1,\"resting\":true}\n";
        gap_out << "{\"seq\":3,\"timestamp_ns\":2,\"type\":\"order_canceled\",\"order_id\":1,\"related_order_id\":0,\"price_ticks\":1,\"quantity_lots\":1,\"remaining_lots\":0,\"reason_code\":0,\"side\":1,\"order_type\":2,\"tif\":1,\"resting\":false}\n";
        gap_out.close();

        argentum::persist::ReplaySummary summary{};
        CHECK(argentum::persist::EventReplayer::replay_file(gap_path, &summary));
        CHECK(summary.sequence_gaps == 1);
        CHECK(summary.gap_positions.size() == 1);
        CHECK(summary.gap_positions.front() == 2);

        argentum::persist::ReplaySummary strict_summary{};
        CHECK(!argentum::persist::EventReplayer::replay_file(
            gap_path,
            &strict_summary,
            argentum::persist::ReplayOptions{.strict_replay = true}));
    }

    std::filesystem::remove(journal_path, ec);
    std::filesystem::remove(gap_path, ec);
    return 0;
}
