#pragma once

#include "core/errors.h"

#include <string>
#include <functional>
#include <vector>
#include <memory>
#include <cstdint>
#include <cstddef>

namespace argentum::bus {

constexpr size_t kMaxMessageSize = 256;

enum class BackpressurePolicy {
    DropNewest = 0,
    DropOldest = 1,
    Block = 2
};

struct InprocBusConfig {
    size_t queue_capacity = 4096;
    BackpressurePolicy policy = BackpressurePolicy::DropNewest;
    uint32_t block_timeout_ms = 0; // 0 = wait indefinitely
    uint32_t consumer_threads = 1;
    std::vector<int> worker_core_ids{};
    int worker_realtime_priority = 0;
    bool disable_cpu_scaling = false;
    bool busy_poll_during_market_hours = true;
    std::string worker_name_prefix = "bus-consumer";
};

struct TopicMetrics {
    uint64_t queue_depth = 0;
    uint64_t drops = 0;
    uint64_t backpressure_hits = 0;
    uint64_t published = 0;
    uint64_t publish_latency_ns_avg = 0;
    uint64_t publish_latency_ns_max = 0;
    // Percentiles from the per-topic publish-latency histogram, sampled
    // 1-in-16 (estimates; counters above stay exact). Zero when the build was
    // configured with ARGENTUM_ENABLE_LATENCY_TRACE=OFF.
    uint64_t publish_latency_ns_p50 = 0;
    uint64_t publish_latency_ns_p95 = 0;
    uint64_t publish_latency_ns_p99 = 0;
    uint64_t publish_latency_ns_p999 = 0;
};

/**
 * @class MessageBus
 * @brief Abstract interface for the low-latency messaging system (ZeroMQ/IPC).
 */
class MessageBus {
public:
    virtual ~MessageBus() = default;

    /**
     * @brief Initialize the bus connection.
     * @param endpoint The connection string (e.g., "tcp://localhost:5555" or "ipc://feeds")
     * @param is_publisher True if this node will write data.
     */
    virtual void connect(const std::string& endpoint, bool is_publisher) = 0;

    /**
     * @brief Publish a binary message to a topic.
     * @param topic The topic string (e.g., "market.btc_usdt").
     * @param data Pointer to data.
     * @param size Size of data.
     * @return Status code indicating backpressure/drop.
     */
    virtual ArgentumStatus publish(const std::string& topic, const void* data, size_t size) = 0;

    /**
     * @brief Subscribe to a topic.
     * @param topic Topic to subscribe to.
     * @param callback Function to handle incoming data.
     */
    virtual void subscribe(const std::string& topic, std::function<void(const void* data, size_t size)> callback) = 0;

    /**
     * @brief Read metrics for a topic.
     * @return true if topic exists.
     */
    virtual bool get_metrics(const std::string& topic, TopicMetrics* out) const = 0;
};

/**
 * @brief Create an in-process MessageBus (thread-safe, single-process).
 * This is the default fallback when ZeroMQ is not linked.
 */
std::shared_ptr<MessageBus> create_inproc_bus(const InprocBusConfig& config);
std::shared_ptr<MessageBus> create_inproc_bus();

} // namespace argentum::bus
