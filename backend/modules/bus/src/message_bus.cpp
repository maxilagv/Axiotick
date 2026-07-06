#include "bus/message_bus.hpp"

#include "audit/logger.hpp"
#include "bus/spsc_ring_buffer.hpp"
#include "core/time_utils.hpp"
#include "system/cpu_utils.hpp"

#include <array>
#include <chrono>
#include <ctime>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <unordered_map>

namespace argentum::bus {

namespace {

constexpr size_t kMaxQueueCapacity = 65536;

bool is_fx_market_open() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t seconds = std::chrono::system_clock::to_time_t(now);
    std::tm utc_tm{};
#ifdef _WIN32
    gmtime_s(&utc_tm, &seconds);
#else
    gmtime_r(&seconds, &utc_tm);
#endif

    const int weekday = utc_tm.tm_wday;
    const int minutes = utc_tm.tm_hour * 60 + utc_tm.tm_min;
    if (weekday >= 1 && weekday <= 4) {
        return true;
    }
    if (weekday == 0) {
        return minutes >= (22 * 60);
    }
    if (weekday == 5) {
        return minutes < (22 * 60);
    }
    return false;
}

} // namespace

class InprocMessageBus final : public MessageBus {
public:
    explicit InprocMessageBus(InprocBusConfig config)
        : config_(std::move(config)) {
        if (config_.queue_capacity == 0) {
            config_.queue_capacity = 1;
        }
        if (config_.queue_capacity > kMaxQueueCapacity) {
            config_.queue_capacity = kMaxQueueCapacity;
        }
        if (config_.consumer_threads > 1) {
            ARGENTUM_LOG(WARN, "[Bus] SPSC bus supports one consumer thread per topic; clamping consumer_threads to 1.");
            config_.consumer_threads = 1;
        }
        if (config_.disable_cpu_scaling) {
            argentum::system::disable_cpu_frequency_scaling();
        }
    }

    ~InprocMessageBus() override {
        shutdown();
    }

    void connect(const std::string& endpoint, bool is_publisher) override {
        endpoint_ = endpoint;
        is_publisher_ = is_publisher;
    }

    ArgentumStatus publish(const std::string& topic, const void* data, size_t size) override {
        if (!data || size == 0 || size > kMaxMessageSize) {
            return ARGENTUM_ERR_INVALID;
        }

        const uint64_t start_ns = argentum::core::now_ns();
        TopicState* state = get_or_create_topic(topic);
        if (!state || !state->running.load(std::memory_order_acquire)) {
            return ARGENTUM_ERR_INVALID;
        }

        auto publish_now = [&] {
            if (!state->ring.try_publish(data, size)) {
                return false;
            }
            state->metrics.queue_depth.fetch_add(1, std::memory_order_relaxed);
            state->metrics.published.fetch_add(1, std::memory_order_relaxed);
            update_publish_latency(state, start_ns);
            return true;
        };

        if (publish_now()) {
            return ARGENTUM_OK;
        }

        state->metrics.backpressure_hits.fetch_add(1, std::memory_order_relaxed);
        switch (config_.policy) {
            case BackpressurePolicy::DropNewest: {
                state->metrics.drops.fetch_add(1, std::memory_order_relaxed);
                update_publish_latency(state, start_ns);
                return ARGENTUM_ERR_TIMEOUT;
            }
            case BackpressurePolicy::DropOldest: {
                size_t dropped_size = 0;
                if (state->ring.try_discard_oldest(&dropped_size)) {
                    (void)dropped_size;
                    state->metrics.drops.fetch_add(1, std::memory_order_relaxed);
                    state->metrics.queue_depth.fetch_sub(1, std::memory_order_relaxed);
                    if (publish_now()) {
                        return ARGENTUM_OK;
                    }
                }
                update_publish_latency(state, start_ns);
                return ARGENTUM_ERR_TIMEOUT;
            }
            case BackpressurePolicy::Block: {
                const auto start = std::chrono::steady_clock::now();
                const auto timeout = std::chrono::milliseconds(config_.block_timeout_ms);
                while (state->running.load(std::memory_order_acquire)) {
                    if (publish_now()) {
                        return ARGENTUM_OK;
                    }
                    if (config_.block_timeout_ms != 0 &&
                        (std::chrono::steady_clock::now() - start) >= timeout) {
                        update_publish_latency(state, start_ns);
                        return ARGENTUM_ERR_TIMEOUT;
                    }

                    if (config_.busy_poll_during_market_hours && is_fx_market_open()) {
                        std::this_thread::yield();
                    } else {
                        std::this_thread::sleep_for(std::chrono::microseconds(50));
                    }
                }
                update_publish_latency(state, start_ns);
                return ARGENTUM_ERR_TIMEOUT;
            }
        }

        update_publish_latency(state, start_ns);
        return ARGENTUM_ERR_TIMEOUT;
    }

    void subscribe(const std::string& topic, std::function<void(const void* data, size_t size)> callback) override {
        TopicState* state = get_or_create_topic(topic);
        if (!state) {
            return;
        }

        {
            std::unique_lock lock(state->subscribers_mutex);
            state->subscribers.push_back(std::move(callback));
        }

        if (config_.consumer_threads > 0) {
            start_consumer(state);
        }
    }

    bool get_metrics(const std::string& topic, TopicMetrics* out) const override {
        if (!out) {
            return false;
        }

        std::shared_lock lock(mutex_);
        const auto it = topics_.find(topic);
        if (it == topics_.end()) {
            return false;
        }

        const TopicMetricsInternal& metrics = it->second->metrics;
        const uint64_t published = metrics.published.load(std::memory_order_relaxed);
        const uint64_t total_latency = metrics.publish_latency_ns_total.load(std::memory_order_relaxed);
        out->queue_depth = metrics.queue_depth.load(std::memory_order_relaxed);
        out->drops = metrics.drops.load(std::memory_order_relaxed);
        out->backpressure_hits = metrics.backpressure_hits.load(std::memory_order_relaxed);
        out->published = published;
        out->publish_latency_ns_avg = (published == 0) ? 0 : (total_latency / published);
        out->publish_latency_ns_max = metrics.publish_latency_ns_max.load(std::memory_order_relaxed);
        return true;
    }

private:
    struct TopicMetricsInternal {
        std::atomic<uint64_t> queue_depth{0};
        std::atomic<uint64_t> drops{0};
        std::atomic<uint64_t> backpressure_hits{0};
        std::atomic<uint64_t> published{0};
        std::atomic<uint64_t> publish_latency_ns_total{0};
        std::atomic<uint64_t> publish_latency_ns_max{0};
    };

    struct TopicState {
        explicit TopicState(size_t queue_capacity)
            : ring(queue_capacity) {}

        mutable std::shared_mutex subscribers_mutex;
        std::vector<std::function<void(const void*, size_t)>> subscribers;
        std::thread worker;
        SpscRingBuffer<kMaxQueueCapacity, kMaxMessageSize> ring;
        TopicMetricsInternal metrics;
        std::atomic<bool> running{true};
        std::atomic<bool> consumer_started{false};
    };

    TopicState* get_or_create_topic(const std::string& topic) {
        {
            std::shared_lock lock(mutex_);
            const auto it = topics_.find(topic);
            if (it != topics_.end()) {
                return it->second.get();
            }
        }

        std::unique_lock lock(mutex_);
        const auto it = topics_.find(topic);
        if (it != topics_.end()) {
            return it->second.get();
        }

        auto state = std::make_unique<TopicState>(config_.queue_capacity);
        TopicState* ptr = state.get();
        topics_[topic] = std::move(state);
        return ptr;
    }

    void start_consumer(TopicState* state) {
        if (!state) {
            return;
        }
        bool expected = false;
        if (!state->consumer_started.compare_exchange_strong(
                expected,
                true,
                std::memory_order_acq_rel,
                std::memory_order_relaxed)) {
            return;
        }

        state->worker = std::thread([this, state] { consumer_loop(state); });
        if (!config_.worker_core_ids.empty()) {
            argentum::system::set_thread_affinity(state->worker, config_.worker_core_ids.front());
        }
        if (config_.worker_realtime_priority > 0) {
            argentum::system::set_thread_realtime_priority(state->worker, config_.worker_realtime_priority);
        }
        argentum::system::set_thread_name(state->worker, config_.worker_name_prefix.c_str());
    }

    void consumer_loop(TopicState* state) {
        std::array<uint8_t, kMaxMessageSize> buffer{};
        size_t idle_spins = 0;

        while (state->running.load(std::memory_order_acquire) || state->ring.size_approx() > 0) {
            size_t message_size = buffer.size();
            if (state->ring.try_consume(buffer.data(), &message_size)) {
                state->metrics.queue_depth.fetch_sub(1, std::memory_order_relaxed);
                idle_spins = 0;

                std::shared_lock lock(state->subscribers_mutex);
                for (auto& callback : state->subscribers) {
                    callback(buffer.data(), message_size);
                }
                continue;
            }

            if (config_.busy_poll_during_market_hours && is_fx_market_open()) {
                ++idle_spins;
                if (idle_spins < 2048) {
                    std::this_thread::yield();
                } else {
                    idle_spins = 0;
                    std::this_thread::sleep_for(std::chrono::microseconds(50));
                }
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
    }

    void update_publish_latency(TopicState* state, uint64_t start_ns) {
        if (!state) {
            return;
        }
        const uint64_t elapsed = argentum::core::now_ns() - start_ns;
        state->metrics.publish_latency_ns_total.fetch_add(elapsed, std::memory_order_relaxed);
        uint64_t prev = state->metrics.publish_latency_ns_max.load(std::memory_order_relaxed);
        while (elapsed > prev &&
               !state->metrics.publish_latency_ns_max.compare_exchange_weak(
                   prev,
                   elapsed,
                   std::memory_order_relaxed,
                   std::memory_order_relaxed)) {
        }
    }

    void shutdown() {
        std::unique_lock lock(mutex_);
        for (auto& [_, state] : topics_) {
            state->running.store(false, std::memory_order_release);
        }
        for (auto& [_, state] : topics_) {
            if (state->worker.joinable()) {
                state->worker.join();
            }
            state->consumer_started.store(false, std::memory_order_relaxed);
        }
    }

    std::string endpoint_;
    bool is_publisher_ = false;
    InprocBusConfig config_;
    std::unordered_map<std::string, std::unique_ptr<TopicState>> topics_;
    mutable std::shared_mutex mutex_;
};

std::shared_ptr<MessageBus> create_inproc_bus(const InprocBusConfig& config) {
    return std::make_shared<InprocMessageBus>(config);
}

std::shared_ptr<MessageBus> create_inproc_bus() {
    return std::make_shared<InprocMessageBus>(InprocBusConfig{});
}

} // namespace argentum::bus
