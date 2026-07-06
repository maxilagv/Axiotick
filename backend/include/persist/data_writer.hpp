#pragma once

#include "core/types.h"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace argentum::persist {

#ifdef ARGENTUM_USE_LIBPQ
struct PGconn;
#endif

class DataWriterService {
public:
    enum class OverflowPolicy {
        DropNewest = 0,
        DropOldest = 1,
        Block = 2
    };

    explicit DataWriterService(std::string connection_string = {});
    ~DataWriterService();

    void start();
    void stop();

    void enqueue(const MarketTick& tick);
    void enqueue_batch(const MarketTick* ticks, size_t count);

    void set_max_batch(size_t max_batch);
    void set_flush_interval_ms(uint32_t ms);
    void set_queue_capacity(size_t capacity);
    void set_overflow_policy(OverflowPolicy policy);

    void set_csv_path(std::string path);
    void set_csv_max_bytes(uint64_t max_bytes);
    void set_csv_fsync(bool enabled);

    uint64_t dropped_count() const;
    uint64_t failed_flush_count() const;

private:
    void worker_loop();
    void flush_batch(std::vector<MarketTick>& batch);
    void flush_csv(std::vector<MarketTick>& batch);

    std::string connection_string_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::condition_variable cv_space_;
    std::deque<MarketTick> queue_;
    size_t max_batch_ = 1024;
    uint32_t flush_interval_ms_ = 100;
    size_t queue_capacity_ = 8192;
    OverflowPolicy overflow_policy_ = OverflowPolicy::DropNewest;
    std::atomic<uint64_t> dropped_{0};
    std::atomic<uint64_t> failed_flushes_{0};

    std::string csv_path_ = "data/market_ticks.csv";
    uint64_t csv_max_bytes_ = 64ULL * 1024ULL * 1024ULL;
    bool csv_fsync_ = false;
    std::atomic<bool> running_{false};
    std::thread worker_;

#ifdef ARGENTUM_USE_LIBPQ
    struct PGconn* pg_conn_ = nullptr;
    uint64_t reconnect_backoff_ms_ = 250;
    uint64_t reconnect_backoff_max_ms_ = 5000;
#endif
};

} // namespace argentum::persist
