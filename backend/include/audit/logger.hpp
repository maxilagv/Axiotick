#pragma once

#include "core/time_utils.hpp"
#include "persist/journal_ring_buffer.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <fstream>
#include <initializer_list>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <utility>

namespace argentum::audit {

#ifdef ERROR
#undef ERROR
#endif

enum class LogLevel : uint8_t {
    TRACE = 0,
    DEBUG = 1,
    INFO = 2,
    WARN = 3,
    ERROR = 4,
    CRITICAL = 5,
    AUDIT = 6
};

class Logger {
public:
    static Logger& instance() {
        static Logger instance;
        return instance;
    }

    void log(LogLevel level, const std::string& message) {
        LogEntry entry{};
        entry.level = level;
        entry.timestamp_ns = argentum::core::unix_now_ns();
        entry.message = message;

        if (!queue_.try_push(std::move(entry))) {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        cv_.notify_one();
    }

    void structured_log(
        LogLevel level,
        const std::string& event,
        std::initializer_list<std::pair<std::string, std::string>> fields = {}) {
        LogEntry entry{};
        entry.level = level;
        entry.timestamp_ns = argentum::core::unix_now_ns();
        entry.structured = true;
        entry.message = build_structured_payload(level, event, fields, entry.timestamp_ns);

        if (!queue_.try_push(std::move(entry))) {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        cv_.notify_one();
    }

    void set_min_level(LogLevel level) {
        min_level_.store(static_cast<uint8_t>(level), std::memory_order_relaxed);
    }

    [[nodiscard]] bool should_log(LogLevel level) const {
        if (level == LogLevel::AUDIT) {
            return true;
        }
        return static_cast<uint8_t>(level) >= min_level_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] uint64_t dropped_count() const {
        return dropped_.load(std::memory_order_relaxed);
    }

private:
    struct LogEntry {
        LogLevel level = LogLevel::INFO;
        uint64_t timestamp_ns = 0;
        bool structured = false;
        std::string message;
    };

    static constexpr size_t kQueueCapacity = 8192;

    Logger() {
#ifdef NDEBUG
        min_level_.store(static_cast<uint8_t>(LogLevel::INFO), std::memory_order_relaxed);
#else
        min_level_.store(static_cast<uint8_t>(LogLevel::TRACE), std::memory_order_relaxed);
#endif
        file_.open("audit.log", std::ios::app);
        worker_ = std::thread(&Logger::worker_loop, this);
    }

    ~Logger() {
        running_.store(false, std::memory_order_release);
        cv_.notify_all();
        if (worker_.joinable()) {
            worker_.join();
        }
        if (file_.is_open()) {
            file_.flush();
            file_.close();
        }
    }

    static const char* to_string(LogLevel level) {
        switch (level) {
            case LogLevel::TRACE: return "TRACE";
            case LogLevel::DEBUG: return "DEBUG";
            case LogLevel::INFO: return "INFO";
            case LogLevel::WARN: return "WARN";
            case LogLevel::ERROR: return "ERROR";
            case LogLevel::CRITICAL: return "CRITICAL";
            case LogLevel::AUDIT: return "AUDIT";
            default: return "UNKNOWN";
        }
    }

    static std::string escape_json(const std::string& raw) {
        std::string out;
        out.reserve(raw.size() + 8);
        for (char c : raw) {
            switch (c) {
                case '"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default: out.push_back(c); break;
            }
        }
        return out;
    }

    static std::string build_structured_payload(
        LogLevel level,
        const std::string& event,
        std::initializer_list<std::pair<std::string, std::string>> fields,
        uint64_t timestamp_ns) {
        std::ostringstream os;
        os << "{\"timestamp_ns\":" << timestamp_ns
           << ",\"level\":\"" << to_string(level) << "\""
           << ",\"event\":\"" << escape_json(event) << "\"";
        for (const auto& [key, value] : fields) {
            os << ",\"" << escape_json(key) << "\":\"" << escape_json(value) << "\"";
        }
        os << "}";
        return os.str();
    }

    static std::string format_plain_entry(const LogEntry& entry) {
        char ts_buf[64];
        argentum::core::format_utc(entry.timestamp_ns, ts_buf, sizeof(ts_buf));
        std::ostringstream os;
        os << ts_buf << " [" << to_string(entry.level) << "] " << entry.message;
        return os.str();
    }

    void worker_loop() {
        while (running_.load(std::memory_order_acquire) || queue_.size_approx() > 0) {
            LogEntry entry{};
            if (!queue_.try_pop(&entry)) {
                std::unique_lock<std::mutex> lock(wait_mutex_);
                cv_.wait_for(lock, std::chrono::milliseconds(1), [&] {
                    return !running_.load(std::memory_order_acquire) || queue_.size_approx() > 0;
                });
                continue;
            }

            const std::string line = entry.structured ? entry.message : format_plain_entry(entry);
            FILE* stream = (entry.level == LogLevel::ERROR || entry.level == LogLevel::CRITICAL)
                ? stderr
                : stdout;
            std::fwrite(line.data(), 1, line.size(), stream);
            std::fwrite("\n", 1, 1, stream);
            std::fflush(stream);
            if (file_.is_open()) {
                file_ << line << '\n';
            }
        }
    }

    persist::LockFreeRingBuffer<LogEntry, kQueueCapacity> queue_;
    std::ofstream file_;
    std::mutex wait_mutex_;
    std::condition_variable cv_;
    std::atomic<uint64_t> dropped_{0};
    std::atomic<uint8_t> min_level_{static_cast<uint8_t>(LogLevel::INFO)};
    std::atomic<bool> running_{true};
    std::thread worker_;
};

namespace detail {

constexpr bool compiled_out(LogLevel level) {
#ifdef NDEBUG
    return level == LogLevel::TRACE || level == LogLevel::DEBUG;
#else
    (void)level;
    return false;
#endif
}

template<typename Builder>
inline void log_with_builder(LogLevel level, Builder&& builder) {
    if (compiled_out(level)) {
        return;
    }
    if (!Logger::instance().should_log(level)) {
        return;
    }

    std::ostringstream stream;
    builder(stream);
    Logger::instance().log(level, stream.str());
}

} // namespace detail

} // namespace argentum::audit

#define ARGENTUM_LOG(level, message_expr) \
    do { \
        ::argentum::audit::detail::log_with_builder( \
            ::argentum::audit::LogLevel::level, \
            [&](std::ostringstream& argentum_log_stream__) { argentum_log_stream__ << message_expr; }); \
    } while (0)
