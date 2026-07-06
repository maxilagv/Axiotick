#include "persist/data_writer.hpp"

#include <chrono>
#include <thread>
#include <filesystem>
#include <cstdio>
#include <sstream>
#include <algorithm>
#include <system_error>
#include <cstdio>
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

#include "core/time_utils.hpp"
#ifdef ARGENTUM_USE_LIBPQ
#include <libpq-fe.h>
#endif

namespace argentum::persist {

DataWriterService::DataWriterService(std::string connection_string)
    : connection_string_(std::move(connection_string)) {}

DataWriterService::~DataWriterService() {
    stop();
}

void DataWriterService::start() {
    if (running_.exchange(true)) return;
    worker_ = std::thread(&DataWriterService::worker_loop, this);
}

void DataWriterService::stop() {
    if (!running_.exchange(false)) return;
    cv_.notify_all();
    cv_space_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
#ifdef ARGENTUM_USE_LIBPQ
    if (pg_conn_) {
        PQfinish(pg_conn_);
        pg_conn_ = nullptr;
    }
#endif
}

void DataWriterService::set_max_batch(size_t max_batch) {
    max_batch_ = (max_batch == 0) ? 1 : max_batch;
}

void DataWriterService::set_flush_interval_ms(uint32_t ms) {
    flush_interval_ms_ = (ms == 0) ? 1 : ms;
}

void DataWriterService::set_queue_capacity(size_t capacity) {
    queue_capacity_ = (capacity == 0) ? 1 : capacity;
}

void DataWriterService::set_overflow_policy(OverflowPolicy policy) {
    overflow_policy_ = policy;
}

void DataWriterService::set_csv_path(std::string path) {
    csv_path_ = std::move(path);
}

void DataWriterService::set_csv_max_bytes(uint64_t max_bytes) {
    csv_max_bytes_ = max_bytes;
}

void DataWriterService::set_csv_fsync(bool enabled) {
    csv_fsync_ = enabled;
}

uint64_t DataWriterService::dropped_count() const {
    return dropped_.load(std::memory_order_relaxed);
}

uint64_t DataWriterService::failed_flush_count() const {
    return failed_flushes_.load(std::memory_order_relaxed);
}

void DataWriterService::enqueue(const MarketTick& tick) {
    std::unique_lock lock(mutex_);
    if (!running_) return;
    if (queue_.size() >= queue_capacity_) {
        switch (overflow_policy_) {
            case OverflowPolicy::DropNewest:
                dropped_.fetch_add(1, std::memory_order_relaxed);
                return;
            case OverflowPolicy::DropOldest:
                if (!queue_.empty()) {
                    queue_.pop_front();
                    dropped_.fetch_add(1, std::memory_order_relaxed);
                }
                break;
            case OverflowPolicy::Block:
                cv_space_.wait(lock, [&] {
                    return !running_ || queue_.size() < queue_capacity_;
                });
                if (!running_) return;
                break;
        }
    }
    queue_.push_back(tick);
    if (queue_.size() >= max_batch_ || queue_.size() == 1) {
        cv_.notify_one();
    }
}

void DataWriterService::enqueue_batch(const MarketTick* ticks, size_t count) {
    if (!ticks || count == 0) return;
    std::unique_lock lock(mutex_);
    if (!running_) return;
    for (size_t i = 0; i < count; ++i) {
        if (queue_.size() >= queue_capacity_) {
            switch (overflow_policy_) {
                case OverflowPolicy::DropNewest:
                    dropped_.fetch_add(1, std::memory_order_relaxed);
                    continue;
                case OverflowPolicy::DropOldest:
                    if (!queue_.empty()) {
                        queue_.pop_front();
                        dropped_.fetch_add(1, std::memory_order_relaxed);
                    }
                    break;
                case OverflowPolicy::Block:
                    cv_space_.wait(lock, [&] {
                        return !running_ || queue_.size() < queue_capacity_;
                    });
                    if (!running_) return;
                    break;
            }
        }
        queue_.push_back(ticks[i]);
    }
    if (queue_.size() >= max_batch_ || !queue_.empty()) {
        cv_.notify_one();
    }
}

void DataWriterService::worker_loop() {
    using namespace std::chrono;
    std::vector<MarketTick> batch;

    while (running_ || !queue_.empty()) {
        {
            std::unique_lock lock(mutex_);
            cv_.wait_for(lock, milliseconds(flush_interval_ms_), [&] {
                return !queue_.empty() || !running_;
            });
            if (queue_.empty()) {
                continue;
            }
            size_t count = std::min(max_batch_, queue_.size());
            batch.clear();
            batch.reserve(count);
            for (size_t i = 0; i < count; ++i) {
                batch.push_back(queue_.front());
                queue_.pop_front();
            }
            cv_space_.notify_all();
        }

        if (!batch.empty()) {
            flush_batch(batch);
        }
    }
}

void DataWriterService::flush_batch(std::vector<MarketTick>& batch) {
#ifdef ARGENTUM_USE_LIBPQ
    if (!pg_conn_ || PQstatus(pg_conn_) != CONNECTION_OK) {
        if (pg_conn_) {
            PQfinish(pg_conn_);
            pg_conn_ = nullptr;
        }
        pg_conn_ = PQconnectdb(connection_string_.c_str());
        if (!pg_conn_ || PQstatus(pg_conn_) != CONNECTION_OK) {
            if (pg_conn_) {
                PQfinish(pg_conn_);
                pg_conn_ = nullptr;
            }
            failed_flushes_.fetch_add(1, std::memory_order_relaxed);
            flush_csv(batch);
            std::this_thread::sleep_for(std::chrono::milliseconds(reconnect_backoff_ms_));
            reconnect_backoff_ms_ = std::min(reconnect_backoff_ms_ * 2, reconnect_backoff_max_ms_);
            return;
        }
        reconnect_backoff_ms_ = 250;
    }

    PGresult* res = PQexec(pg_conn_,
        "COPY market_ticks (time, symbol, price, volume, side, source) "
        "FROM STDIN WITH (FORMAT csv)");
    if (!res || PQresultStatus(res) != PGRES_COPY_IN) {
        if (res) PQclear(res);
        failed_flushes_.fetch_add(1, std::memory_order_relaxed);
        flush_csv(batch);
        return;
    }
    PQclear(res);

    char line[320];
    char ts_buf[64];
    bool copy_failed = false;
    for (const auto& tick : batch) {
        argentum::core::format_utc(tick.timestamp_ns, ts_buf, sizeof(ts_buf));
        int written = snprintf(line, sizeof(line),
            "%s,%s,%.10f,%.10f,%c,%s\n",
            ts_buf,
            tick.symbol,
            tick.price,
            tick.quantity,
            tick.side == SIDE_BUY ? 'B' : 'S',
            tick.source);
        if (written > 0) {
            if (PQputCopyData(pg_conn_, line, written) != 1) {
                copy_failed = true;
                break;
            }
        }
    }
    if (PQputCopyEnd(pg_conn_, copy_failed ? "copy data failed" : NULL) != 1) {
        copy_failed = true;
    }

    bool result_failed = false;
    while ((res = PQgetResult(pg_conn_)) != NULL) {
        if (PQresultStatus(res) != PGRES_COMMAND_OK) {
            result_failed = true;
        }
        PQclear(res);
    }

    if (copy_failed || result_failed) {
        failed_flushes_.fetch_add(1, std::memory_order_relaxed);
        flush_csv(batch);
    }
#else
    flush_csv(batch);
#endif
}

void DataWriterService::flush_csv(std::vector<MarketTick>& batch) {
    if (batch.empty()) return;
    std::filesystem::path path(csv_path_);
    std::error_code ec;
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path(), ec);
    }

    bool write_header = false;
    if (std::filesystem::exists(path, ec) && !ec) {
        auto size = std::filesystem::file_size(path, ec);
        if (!ec && size >= csv_max_bytes_) {
            auto ts = argentum::core::to_utc(argentum::core::unix_now_ns());
            for (char& c : ts) {
                if (c == ' ' || c == ':' || c == '+') c = '_';
            }
            std::filesystem::path rotated = path.parent_path() /
                (path.stem().string() + "_" + ts + path.extension().string());
            std::filesystem::rename(path, rotated, ec);
            write_header = true;
        }
    } else {
        write_header = true;
    }

    FILE* file = std::fopen(path.string().c_str(), "ab");
    if (!file) return;

    if (write_header) {
        std::fputs("timestamp_ns,symbol,price,quantity,side,source\n", file);
    }

    for (const auto& tick : batch) {
        std::fprintf(file, "%llu,%s,%.10f,%.10f,%c,%s\n",
                     static_cast<unsigned long long>(tick.timestamp_ns),
                     tick.symbol,
                     tick.price,
                     tick.quantity,
                     (tick.side == SIDE_BUY ? 'B' : 'S'),
                     tick.source);
    }
    std::fflush(file);
    if (csv_fsync_) {
#ifdef _WIN32
        _commit(_fileno(file));
#else
        fsync(fileno(file));
#endif
    }
    std::fclose(file);
}

} // namespace argentum::persist
