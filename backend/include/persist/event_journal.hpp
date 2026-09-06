#pragma once

#include "core/latency_histogram.hpp"
#include "core/types.h"
#include "persist/journal_ring_buffer.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#ifdef ERROR
#undef ERROR
#endif
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif
#else
#include <sys/select.h>
#include <unistd.h>
#endif

namespace argentum::persist {

enum class JournalEventType : uint8_t {
    OrderAccepted = 1,
    OrderRejected = 2,
    TradeExecuted = 3,
    OrderCanceled = 4,
    OrderReplaced = 5,
    GatewayRejected = 6
};

struct JournalEvent {
    uint64_t seq = 0;
    uint64_t timestamp_ns = 0;
    JournalEventType type = JournalEventType::OrderAccepted;
    uint64_t order_id = 0;
    uint64_t related_order_id = 0;
    int64_t price_ticks = 0;
    int64_t quantity_lots = 0;
    int64_t remaining_lots = 0;
    int32_t reason_code = 0;
    uint8_t side = 0;
    uint8_t order_type = 0;
    uint8_t tif = 0;
    bool resting = false;
    uint64_t related_signal_id = 0;  // correlates the order with the signal_decision audit event
    uint64_t tick_id = 0;            // trace id of the originating market tick (0 = unknown/legacy)
    uint64_t enqueued_at_ns = 0;
};

template<size_t N>
using JournalRingBuffer = LockFreeRingBuffer<JournalEvent, N>;

struct JournalLatencySnapshot {
    uint64_t samples = 0;
    uint64_t p50_ns = 0;
    uint64_t p95_ns = 0;
    uint64_t p99_ns = 0;
    uint64_t p999_ns = 0;
    uint64_t max_ns = 0;
};

class AsyncEventJournal {
public:
    static constexpr size_t kDefaultRingBufferSize = 65536;

    explicit AsyncEventJournal(std::string path = "data/order_events.jsonl");
    ~AsyncEventJournal();

    bool append(const JournalEvent& event);
    bool append(JournalEvent&& event);
    bool append_external(const JournalEvent& event);
    bool append_external(JournalEvent&& event);
    void flush();
    const std::string& path() const;

    [[nodiscard]] uint64_t dropped_events() const;
    [[nodiscard]] uint64_t written_events() const;
    [[nodiscard]] uint64_t last_written_sequence() const;
    [[nodiscard]] size_t pending_events() const;
    [[nodiscard]] JournalLatencySnapshot latency_snapshot() const;

private:
    bool append_impl(JournalEvent&& event);
    JournalEvent prepare_event(JournalEvent&& event);
    void worker_loop();
    void write_event(const JournalEvent& event);
    void record_latency_sample(uint64_t latency_ns);
    void wake_worker();
    void wait_for_work();
    void signal_flush_progress(uint64_t sequence);
    void close_wait_handle();

    std::string path_;
    std::ofstream file_;
    std::vector<char> write_buffer_;
    std::unique_ptr<JournalRingBuffer<kDefaultRingBufferSize>> ring_buffer_;
    std::atomic<bool> running_{true};
    std::thread worker_;
    std::atomic<uint64_t> next_seq_{1};
    std::atomic<uint64_t> last_timestamp_ns_{0};
    std::atomic<uint64_t> dropped_events_{0};
    std::atomic<uint64_t> written_events_{0};
    std::atomic<uint64_t> last_written_seq_{0};
    mutable std::mutex flush_mutex_;
    std::condition_variable flush_cv_;
    mutable std::mutex latency_mutex_;
    core::LatencyHistogram latency_hist_;

#ifdef _WIN32
    HANDLE wake_event_ = nullptr;
#else
    int wake_pipe_read_ = -1;
    int wake_pipe_write_ = -1;
#endif
};

using EventJournal = AsyncEventJournal;

struct ReplayOrderState {
    int64_t price_ticks = 0;
    int64_t initial_lots = 0;
    int64_t remaining_lots = 0;
    int64_t filled_lots = 0;
    uint8_t side = 0;
    uint8_t status = 0;
};

struct ReplaySummary {
    uint64_t total_events = 0;
    uint64_t accepted = 0;
    uint64_t rejected = 0;
    uint64_t gateway_rejected = 0;
    uint64_t trades = 0;
    uint64_t canceled = 0;
    uint64_t replaced = 0;
    uint64_t sequence_gaps = 0;
    bool monotonic_seq = true;
    bool monotonic_time = true;
    int64_t committed_exposure_units = 0;
    int64_t filled_exposure_units = 0;
    int64_t net_position_lots = 0;
    std::vector<uint64_t> gap_positions;
    std::unordered_map<uint64_t, ReplayOrderState> active_orders;
    std::unordered_map<uint64_t, ReplayOrderState> order_history;
};

struct ReplayOptions {
    bool strict_replay = false;
};

const char* journal_event_type_to_string(JournalEventType type);
bool journal_event_type_from_string(const std::string& raw, JournalEventType* out);

class EventReplayer {
public:
    static bool replay_file(
        const std::string& path,
        ReplaySummary* out_summary,
        ReplayOptions options = {});
};

} // namespace argentum::persist
