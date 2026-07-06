#include "persist/event_journal.hpp"

#include "audit/logger.hpp"
#include "core/fixed_point.hpp"
#include "core/time_utils.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <sstream>
#include <stdexcept>

#ifndef _WIN32
#include <fcntl.h>
#endif

namespace argentum::persist {

namespace {

const char* kUnknownType = "unknown";

[[noreturn]] void fail_fast_open(const std::string& path) {
#if defined(_CPPUNWIND) || defined(__cpp_exceptions)
    throw std::runtime_error("failed to open event journal: " + path);
#else
    std::fprintf(stderr, "failed to open event journal: %s\n", path.c_str());
    std::abort();
#endif
}

bool parse_u64_field(const std::string& json, const std::string& key, uint64_t* out) {
    if (!out) return false;
    const std::string marker = "\"" + key + "\":";
    const size_t pos = json.find(marker);
    if (pos == std::string::npos) return false;

    size_t begin = pos + marker.size();
    while (begin < json.size() && (json[begin] == ' ' || json[begin] == '\t')) ++begin;
    size_t end = begin;
    while (end < json.size() && json[end] >= '0' && json[end] <= '9') ++end;
    if (end == begin) return false;

    *out = static_cast<uint64_t>(std::strtoull(json.substr(begin, end - begin).c_str(), nullptr, 10));
    return true;
}

bool parse_i64_field(const std::string& json, const std::string& key, int64_t* out) {
    if (!out) return false;
    const std::string marker = "\"" + key + "\":";
    const size_t pos = json.find(marker);
    if (pos == std::string::npos) return false;

    size_t begin = pos + marker.size();
    while (begin < json.size() && (json[begin] == ' ' || json[begin] == '\t')) ++begin;
    size_t end = begin;
    if (end < json.size() && json[end] == '-') ++end;
    const size_t digits_begin = end;
    while (end < json.size() && json[end] >= '0' && json[end] <= '9') ++end;
    if (digits_begin == end) return false;

    *out = static_cast<int64_t>(std::strtoll(json.substr(begin, end - begin).c_str(), nullptr, 10));
    return true;
}

bool parse_i32_field(const std::string& json, const std::string& key, int32_t* out) {
    if (!out) return false;
    int64_t tmp = 0;
    if (!parse_i64_field(json, key, &tmp)) return false;
    *out = static_cast<int32_t>(tmp);
    return true;
}

bool parse_bool_field(const std::string& json, const std::string& key, bool* out) {
    if (!out) return false;
    const std::string marker = "\"" + key + "\":";
    const size_t pos = json.find(marker);
    if (pos == std::string::npos) return false;

    size_t begin = pos + marker.size();
    while (begin < json.size() && (json[begin] == ' ' || json[begin] == '\t')) ++begin;

    if (json.compare(begin, 4, "true") == 0) {
        *out = true;
        return true;
    }
    if (json.compare(begin, 5, "false") == 0) {
        *out = false;
        return true;
    }
    return false;
}

bool parse_string_field(const std::string& json, const std::string& key, std::string* out) {
    if (!out) return false;
    const std::string marker = "\"" + key + "\":\"";
    const size_t pos = json.find(marker);
    if (pos == std::string::npos) return false;

    const size_t begin = pos + marker.size();
    size_t end = begin;
    while (end < json.size()) {
        if (json[end] == '"' && (end == begin || json[end - 1] != '\\')) break;
        ++end;
    }
    if (end >= json.size()) return false;

    *out = json.substr(begin, end - begin);
    return true;
}

uint8_t status_for_replay_resting() {
    return 1;
}

uint8_t status_for_replay_partially_filled() {
    return 2;
}

uint8_t status_for_replay_filled() {
    return 3;
}

uint8_t status_for_replay_canceled() {
    return 4;
}

uint8_t status_for_replay_rejected() {
    return 5;
}

int64_t signed_notional_units(int64_t price_ticks, int64_t quantity_lots, uint8_t side) {
    if (price_ticks <= 0 || quantity_lots <= 0) return 0;
    int64_t units = core::to_notional_units(price_ticks, quantity_lots);
    if (side == SIDE_SELL) {
        units = -units;
    }
    return units;
}

void apply_fill_to_state(ReplayOrderState* state, int64_t fill_lots) {
    if (!state || fill_lots <= 0) return;

    const int64_t applied = std::min(fill_lots, std::max<int64_t>(0, state->remaining_lots));
    if (applied <= 0) return;

    state->filled_lots += applied;
    state->remaining_lots = std::max<int64_t>(0, state->remaining_lots - applied);
    if (state->remaining_lots == 0) {
        state->status = status_for_replay_filled();
    } else {
        state->status = status_for_replay_partially_filled();
    }
}

bool load_tail_stats(const std::string& path, uint64_t* out_last_seq, uint64_t* out_last_ts) {
    if (!out_last_seq || !out_last_ts) return false;
    *out_last_seq = 0;
    *out_last_ts = 0;

    std::ifstream in(path);
    if (!in.is_open()) return true;

    std::string line;
    uint64_t max_seq = 0;
    uint64_t max_ts = 0;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        uint64_t seq = 0;
        uint64_t ts = 0;
        if (parse_u64_field(line, "seq", &seq)) {
            max_seq = std::max(max_seq, seq);
        }
        if (parse_u64_field(line, "timestamp_ns", &ts)) {
            max_ts = std::max(max_ts, ts);
        }
    }

    *out_last_seq = max_seq;
    *out_last_ts = max_ts;
    return true;
}

uint64_t percentile_value(std::vector<uint64_t> values, double percentile) {
    if (values.empty()) {
        return 0;
    }

    std::sort(values.begin(), values.end());
    const double raw_index = percentile * static_cast<double>(values.size() - 1);
    const size_t index = static_cast<size_t>(raw_index);
    return values[index];
}

} // namespace

AsyncEventJournal::AsyncEventJournal(std::string path)
    : path_(std::move(path)),
      write_buffer_(1 << 20),
      ring_buffer_(std::make_unique<JournalRingBuffer<kDefaultRingBufferSize>>()) {
    std::error_code ec;
    const std::filesystem::path fs_path(path_);
    if (!fs_path.parent_path().empty()) {
        std::filesystem::create_directories(fs_path.parent_path(), ec);
    }

    uint64_t last_seq = 0;
    uint64_t last_ts = 0;
    if (load_tail_stats(path_, &last_seq, &last_ts)) {
        next_seq_.store(last_seq + 1, std::memory_order_relaxed);
        last_timestamp_ns_.store(last_ts, std::memory_order_relaxed);
    }

    file_.rdbuf()->pubsetbuf(write_buffer_.data(), static_cast<std::streamsize>(write_buffer_.size()));
    file_.open(path_, std::ios::out | std::ios::app);
    if (!file_.is_open()) {
        fail_fast_open(path_);
    }

#ifdef _WIN32
    wake_event_ = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    if (wake_event_ == nullptr) {
        fail_fast_open(path_ + ":wake_event");
    }
#else
    int pipe_fds[2] = {-1, -1};
    if (pipe(pipe_fds) != 0) {
        fail_fast_open(path_ + ":wake_pipe");
    }
    wake_pipe_read_ = pipe_fds[0];
    wake_pipe_write_ = pipe_fds[1];
    (void)fcntl(wake_pipe_read_, F_SETFL, O_NONBLOCK);
    (void)fcntl(wake_pipe_write_, F_SETFL, O_NONBLOCK);
#endif

    latency_samples_.reserve(4096);
    worker_ = std::thread(&AsyncEventJournal::worker_loop, this);
}

AsyncEventJournal::~AsyncEventJournal() {
    flush();
    running_.store(false, std::memory_order_release);
    wake_worker();
    if (worker_.joinable()) {
        worker_.join();
    }
    if (file_.is_open()) {
        file_.flush();
        file_.close();
    }
    close_wait_handle();
}

bool AsyncEventJournal::append(const JournalEvent& event) {
    return append_impl(JournalEvent(event));
}

bool AsyncEventJournal::append(JournalEvent&& event) {
    return append_impl(std::move(event));
}

bool AsyncEventJournal::append_external(const JournalEvent& event) {
    return append_impl(JournalEvent(event));
}

bool AsyncEventJournal::append_external(JournalEvent&& event) {
    return append_impl(std::move(event));
}

void AsyncEventJournal::flush() {
    const uint64_t target_sequence = next_seq_.load(std::memory_order_acquire);
    if (target_sequence <= 1) {
        if (file_.is_open()) {
            file_.flush();
        }
        return;
    }

    wake_worker();
    std::unique_lock<std::mutex> lock(flush_mutex_);
    flush_cv_.wait(lock, [&] {
        return last_written_seq_.load(std::memory_order_acquire) >= (target_sequence - 1);
    });

    if (file_.is_open()) {
        file_.flush();
    }
}

const std::string& AsyncEventJournal::path() const {
    return path_;
}

uint64_t AsyncEventJournal::dropped_events() const {
    return dropped_events_.load(std::memory_order_relaxed);
}

uint64_t AsyncEventJournal::written_events() const {
    return written_events_.load(std::memory_order_relaxed);
}

uint64_t AsyncEventJournal::last_written_sequence() const {
    return last_written_seq_.load(std::memory_order_acquire);
}

size_t AsyncEventJournal::pending_events() const {
    return ring_buffer_->size_approx();
}

JournalLatencySnapshot AsyncEventJournal::latency_snapshot() const {
    std::lock_guard<std::mutex> lock(latency_mutex_);
    JournalLatencySnapshot snapshot{};
    snapshot.samples = latency_samples_.size();
    if (latency_samples_.empty()) {
        return snapshot;
    }

    snapshot.p50_ns = percentile_value(latency_samples_, 0.50);
    snapshot.p95_ns = percentile_value(latency_samples_, 0.95);
    snapshot.p99_ns = percentile_value(latency_samples_, 0.99);
    snapshot.max_ns = *std::max_element(latency_samples_.begin(), latency_samples_.end());
    return snapshot;
}

bool AsyncEventJournal::append_impl(JournalEvent&& event) {
    JournalEvent prepared = prepare_event(std::move(event));
    if (!ring_buffer_->try_push(std::move(prepared))) {
        dropped_events_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    wake_worker();
    return true;
}

JournalEvent AsyncEventJournal::prepare_event(JournalEvent&& event) {
    if (event.seq == 0) {
        event.seq = next_seq_.fetch_add(1, std::memory_order_relaxed);
    } else {
        uint64_t expected = next_seq_.load(std::memory_order_relaxed);
        const uint64_t desired = event.seq + 1;
        while (expected < desired &&
               !next_seq_.compare_exchange_weak(
                   expected,
                   desired,
                   std::memory_order_relaxed,
                   std::memory_order_relaxed)) {
        }
    }

    uint64_t timestamp_ns = event.timestamp_ns;
    if (timestamp_ns == 0) {
        timestamp_ns = core::unix_now_ns();
    }

    uint64_t previous = last_timestamp_ns_.load(std::memory_order_relaxed);
    for (;;) {
        uint64_t normalized = timestamp_ns;
        if (previous != 0 && normalized <= previous) {
            normalized = previous + 1;
        }

        if (last_timestamp_ns_.compare_exchange_weak(
                previous,
                normalized,
                std::memory_order_release,
                std::memory_order_relaxed)) {
            event.timestamp_ns = normalized;
            break;
        }
    }

    event.enqueued_at_ns = core::now_ns();
    return event;
}

void AsyncEventJournal::worker_loop() {
    while (running_.load(std::memory_order_acquire) || ring_buffer_->size_approx() > 0) {
        JournalEvent event{};
        if (!ring_buffer_->try_pop(&event)) {
            wait_for_work();
            continue;
        }

        write_event(event);
    }

    if (file_.is_open()) {
        file_.flush();
    }
    signal_flush_progress(last_written_seq_.load(std::memory_order_acquire));
}

void AsyncEventJournal::write_event(const JournalEvent& event) {
    file_ << "{\"seq\":" << event.seq
          << ",\"timestamp_ns\":" << event.timestamp_ns
          << ",\"type\":\"" << journal_event_type_to_string(event.type) << "\""
          << ",\"order_id\":" << event.order_id
          << ",\"related_order_id\":" << event.related_order_id
          << ",\"related_signal_id\":" << event.related_signal_id
          << ",\"price_ticks\":" << event.price_ticks
          << ",\"quantity_lots\":" << event.quantity_lots
          << ",\"remaining_lots\":" << event.remaining_lots
          << ",\"reason_code\":" << event.reason_code
          << ",\"side\":" << static_cast<uint32_t>(event.side)
          << ",\"order_type\":" << static_cast<uint32_t>(event.order_type)
          << ",\"tif\":" << static_cast<uint32_t>(event.tif)
          << ",\"resting\":" << (event.resting ? "true" : "false")
          << "}\n";

    if (!file_.good()) {
        ARGENTUM_LOG(CRITICAL, "[Journal] write failure path=" << path_ << " seq=" << event.seq);
    }

    if (event.enqueued_at_ns != 0) {
        const uint64_t written_at_ns = core::now_ns();
        if (written_at_ns >= event.enqueued_at_ns) {
            record_latency_sample(written_at_ns - event.enqueued_at_ns);
        }
    }

    written_events_.fetch_add(1, std::memory_order_relaxed);
    last_written_seq_.store(event.seq, std::memory_order_release);
    signal_flush_progress(event.seq);
}

void AsyncEventJournal::record_latency_sample(uint64_t latency_ns) {
    std::lock_guard<std::mutex> lock(latency_mutex_);
    constexpr size_t kMaxSamples = 4096;
    if (latency_samples_.size() >= kMaxSamples) {
        latency_samples_.erase(latency_samples_.begin());
    }
    latency_samples_.push_back(latency_ns);
}

void AsyncEventJournal::wake_worker() {
#ifdef _WIN32
    if (wake_event_ != nullptr) {
        (void)SetEvent(wake_event_);
    }
#else
    if (wake_pipe_write_ >= 0) {
        const unsigned char byte = 1;
        const ssize_t ignored = write(wake_pipe_write_, &byte, sizeof(byte));
        (void)ignored;
    }
#endif
}

void AsyncEventJournal::wait_for_work() {
#ifdef _WIN32
    if (wake_event_ != nullptr) {
        (void)WaitForSingleObject(wake_event_, 1);
    }
#else
    if (wake_pipe_read_ >= 0) {
        fd_set read_set;
        FD_ZERO(&read_set);
        FD_SET(wake_pipe_read_, &read_set);

        timeval timeout{};
        timeout.tv_sec = 0;
        timeout.tv_usec = 1000;
        const int ready = select(wake_pipe_read_ + 1, &read_set, nullptr, nullptr, &timeout);
        if (ready > 0 && FD_ISSET(wake_pipe_read_, &read_set)) {
            unsigned char buffer[64];
            while (read(wake_pipe_read_, buffer, sizeof(buffer)) > 0) {
            }
        }
    }
#endif
}

void AsyncEventJournal::signal_flush_progress(uint64_t sequence) {
    (void)sequence;
    std::lock_guard<std::mutex> lock(flush_mutex_);
    flush_cv_.notify_all();
}

void AsyncEventJournal::close_wait_handle() {
#ifdef _WIN32
    if (wake_event_ != nullptr) {
        CloseHandle(wake_event_);
        wake_event_ = nullptr;
    }
#else
    if (wake_pipe_read_ >= 0) {
        close(wake_pipe_read_);
        wake_pipe_read_ = -1;
    }
    if (wake_pipe_write_ >= 0) {
        close(wake_pipe_write_);
        wake_pipe_write_ = -1;
    }
#endif
}

const char* journal_event_type_to_string(JournalEventType type) {
    switch (type) {
        case JournalEventType::OrderAccepted: return "order_accepted";
        case JournalEventType::OrderRejected: return "order_rejected";
        case JournalEventType::TradeExecuted: return "trade_executed";
        case JournalEventType::OrderCanceled: return "order_canceled";
        case JournalEventType::OrderReplaced: return "order_replaced";
        case JournalEventType::GatewayRejected: return "gateway_rejected";
        default: return kUnknownType;
    }
}

bool journal_event_type_from_string(const std::string& raw, JournalEventType* out) {
    if (!out) return false;
    if (raw == "order_accepted") {
        *out = JournalEventType::OrderAccepted;
        return true;
    }
    if (raw == "order_rejected") {
        *out = JournalEventType::OrderRejected;
        return true;
    }
    if (raw == "trade_executed") {
        *out = JournalEventType::TradeExecuted;
        return true;
    }
    if (raw == "order_canceled") {
        *out = JournalEventType::OrderCanceled;
        return true;
    }
    if (raw == "order_replaced") {
        *out = JournalEventType::OrderReplaced;
        return true;
    }
    if (raw == "gateway_rejected") {
        *out = JournalEventType::GatewayRejected;
        return true;
    }
    return false;
}

bool EventReplayer::replay_file(
    const std::string& path,
    ReplaySummary* out_summary,
    ReplayOptions options) {
    if (!out_summary) return false;

    std::ifstream in(path);
    if (!in.is_open()) return false;

    ReplaySummary summary{};
    uint64_t last_seq = 0;
    uint64_t last_ts = 0;
    std::unordered_map<uint64_t, int64_t> seen_fill_lots;
    std::string line;

    auto apply_fill = [&](uint64_t order_id, int64_t filled_lots) {
        if (order_id == 0 || filled_lots <= 0) return;

        seen_fill_lots[order_id] += filled_lots;
        auto it = summary.active_orders.find(order_id);
        if (it == summary.active_orders.end()) return;

        apply_fill_to_state(&it->second, filled_lots);
        summary.order_history[order_id] = it->second;
        if (it->second.remaining_lots <= 0) {
            summary.active_orders.erase(it);
        }
    };

    while (std::getline(in, line)) {
        if (line.empty()) continue;
        ++summary.total_events;

        JournalEvent event{};
        uint64_t seq = 0;
        uint64_t ts = 0;
        std::string type_raw;
        if (!parse_u64_field(line, "seq", &seq)) return false;
        if (!parse_u64_field(line, "timestamp_ns", &ts)) return false;
        if (!parse_string_field(line, "type", &type_raw)) return false;
        if (!journal_event_type_from_string(type_raw, &event.type)) return false;

        event.seq = seq;
        event.timestamp_ns = ts;
        (void)parse_u64_field(line, "order_id", &event.order_id);
        (void)parse_u64_field(line, "related_order_id", &event.related_order_id);
        (void)parse_u64_field(line, "related_signal_id", &event.related_signal_id);
        (void)parse_i64_field(line, "price_ticks", &event.price_ticks);
        (void)parse_i64_field(line, "quantity_lots", &event.quantity_lots);
        (void)parse_i64_field(line, "remaining_lots", &event.remaining_lots);
        (void)parse_i32_field(line, "reason_code", &event.reason_code);

        uint64_t tmp = 0;
        (void)parse_u64_field(line, "side", &tmp);
        event.side = static_cast<uint8_t>(tmp);
        tmp = 0;
        (void)parse_u64_field(line, "order_type", &tmp);
        event.order_type = static_cast<uint8_t>(tmp);
        tmp = 0;
        (void)parse_u64_field(line, "tif", &tmp);
        event.tif = static_cast<uint8_t>(tmp);
        (void)parse_bool_field(line, "resting", &event.resting);

        if (last_seq != 0) {
            if (seq <= last_seq) {
                summary.monotonic_seq = false;
            }
            if (seq != last_seq + 1) {
                ++summary.sequence_gaps;
                summary.gap_positions.push_back(last_seq + 1);
                if (options.strict_replay) {
                    ARGENTUM_LOG(
                        CRITICAL,
                        "[Replay] sequence gap detected path=" << path
                        << " expected_seq=" << (last_seq + 1)
                        << " actual_seq=" << seq);
                    return false;
                }
            }
        }
        if (last_ts != 0 && ts < last_ts) summary.monotonic_time = false;
        last_seq = seq;
        last_ts = ts;

        switch (event.type) {
            case JournalEventType::OrderAccepted: {
                ++summary.accepted;
                ReplayOrderState st{};
                st.price_ticks = event.price_ticks;
                st.initial_lots = std::max<int64_t>(0, event.quantity_lots);
                st.remaining_lots = std::max<int64_t>(0, event.remaining_lots);
                st.side = event.side;

                const int64_t seen_fills = std::max<int64_t>(0, seen_fill_lots[event.order_id]);
                st.filled_lots = std::min(st.initial_lots, seen_fills);

                if (event.resting && st.remaining_lots > 0) {
                    st.status = (st.filled_lots > 0)
                        ? status_for_replay_partially_filled()
                        : status_for_replay_resting();
                    summary.active_orders[event.order_id] = st;
                } else {
                    st.remaining_lots = 0;
                    if (st.filled_lots >= st.initial_lots && st.initial_lots > 0) {
                        st.status = status_for_replay_filled();
                    } else if (st.filled_lots > 0) {
                        st.status = status_for_replay_partially_filled();
                    } else {
                        st.status = status_for_replay_canceled();
                    }
                    summary.active_orders.erase(event.order_id);
                }

                summary.order_history[event.order_id] = st;
                break;
            }
            case JournalEventType::OrderRejected: {
                ++summary.rejected;
                ReplayOrderState st{};
                st.price_ticks = event.price_ticks;
                st.initial_lots = std::max<int64_t>(0, event.quantity_lots);
                st.remaining_lots = std::max<int64_t>(0, event.remaining_lots);
                st.filled_lots = 0;
                st.side = event.side;
                st.status = status_for_replay_rejected();
                summary.order_history[event.order_id] = st;
                summary.active_orders.erase(event.order_id);
                break;
            }
            case JournalEventType::GatewayRejected: {
                ++summary.gateway_rejected;
                break;
            }
            case JournalEventType::TradeExecuted: {
                ++summary.trades;
                apply_fill(event.order_id, event.quantity_lots);
                if (event.related_order_id != 0) {
                    apply_fill(event.related_order_id, event.quantity_lots);
                }

                summary.filled_exposure_units += signed_notional_units(
                    event.price_ticks,
                    event.quantity_lots,
                    event.side);
                summary.net_position_lots += (event.side == SIDE_BUY)
                    ? event.quantity_lots
                    : -event.quantity_lots;

                if (event.related_order_id != 0) {
                    const uint8_t maker_side = static_cast<uint8_t>(
                        (event.side == SIDE_BUY) ? SIDE_SELL : SIDE_BUY);
                    summary.filled_exposure_units += signed_notional_units(
                        event.price_ticks,
                        event.quantity_lots,
                        maker_side);
                    summary.net_position_lots += (maker_side == SIDE_BUY)
                        ? event.quantity_lots
                        : -event.quantity_lots;
                }
                break;
            }
            case JournalEventType::OrderCanceled: {
                ++summary.canceled;
                auto it = summary.order_history.find(event.order_id);
                if (it != summary.order_history.end()) {
                    it->second.remaining_lots = 0;
                    it->second.status = status_for_replay_canceled();
                    summary.order_history[event.order_id] = it->second;
                } else {
                    ReplayOrderState st{};
                    st.price_ticks = event.price_ticks;
                    st.side = event.side;
                    st.status = status_for_replay_canceled();
                    summary.order_history[event.order_id] = st;
                }
                summary.active_orders.erase(event.order_id);
                break;
            }
            case JournalEventType::OrderReplaced: {
                ++summary.replaced;
                ReplayOrderState st{};
                auto it = summary.order_history.find(event.order_id);
                if (it != summary.order_history.end()) {
                    st = it->second;
                }

                st.price_ticks = event.price_ticks;
                if (event.side == SIDE_BUY || event.side == SIDE_SELL) {
                    st.side = event.side;
                }

                const int64_t new_initial_lots = std::max<int64_t>(0, event.quantity_lots);
                const int64_t new_remaining_lots = std::max<int64_t>(0, event.remaining_lots);
                const int64_t seen_fills_for_order = std::max<int64_t>(0, seen_fill_lots[event.order_id]);

                st.initial_lots = new_initial_lots;
                st.remaining_lots = new_remaining_lots;
                st.filled_lots = std::min(new_initial_lots, std::max<int64_t>(seen_fills_for_order, new_initial_lots - new_remaining_lots));

                if (new_remaining_lots > 0) {
                    st.status = (st.filled_lots > 0)
                        ? status_for_replay_partially_filled()
                        : status_for_replay_resting();
                    summary.active_orders[event.order_id] = st;
                } else {
                    st.status = (st.filled_lots > 0)
                        ? status_for_replay_partially_filled()
                        : status_for_replay_canceled();
                    summary.active_orders.erase(event.order_id);
                }

                summary.order_history[event.order_id] = st;
                break;
            }
        }
    }

    for (const auto& [order_id, state] : summary.active_orders) {
        summary.committed_exposure_units += signed_notional_units(
            state.price_ticks,
            state.remaining_lots,
            state.side);
        summary.order_history[order_id] = state;
    }

    *out_summary = std::move(summary);
    return true;
}

} // namespace argentum::persist
