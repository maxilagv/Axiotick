#pragma once

#include "bus/message_bus.hpp"
#include "core/types.h"
#include "trading/order_manager.hpp"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace argentum::persist {
class AsyncEventJournal;
using EventJournal = AsyncEventJournal;
}

namespace argentum::api {

struct RateLimitConfig {
    uint32_t max_requests = 120;
    uint32_t window_ms = 1000;
};

struct GatewaySecurityConfig {
    std::string api_token;
    RateLimitConfig rate_limit{};
    uint64_t default_token_ttl_ms = 0;
};

enum class GatewayRejectReason {
    None = 0,
    Unauthorized = 1,
    RateLimited = 2
};

struct GatewayMetrics {
    uint64_t ticks_received = 0;
    uint64_t ticks_decoded = 0;
    uint64_t decode_errors = 0;
    uint64_t order_requests = 0;
    uint64_t order_accepted = 0;
    uint64_t order_rejected = 0;
    uint64_t auth_failures = 0;
    uint64_t rate_limited = 0;
    uint64_t tracked_symbols = 0;
    uint64_t journal_written = 0;
    uint64_t journal_dropped = 0;
    uint64_t journal_pending = 0;
    uint64_t journal_latency_p50_ns = 0;
    uint64_t journal_latency_p95_ns = 0;
    uint64_t journal_latency_p99_ns = 0;
};

struct OrderAck {
    uint64_t order_id = 0;
    bool accepted = false;
    bool resting = false;
    double filled_quantity = 0.0;
    double remaining_quantity = 0.0;
    trading::OrderRejectReason reject_reason = trading::OrderRejectReason::None;
    GatewayRejectReason gateway_reject_reason = GatewayRejectReason::None;
};

class MarketGatewayService {
public:
    MarketGatewayService(
        std::shared_ptr<bus::MessageBus> bus,
        std::string market_topic = "market.ticks",
        GatewaySecurityConfig security = {},
        std::shared_ptr<persist::EventJournal> journal = nullptr);

    void start();
    void stop();

    bool get_latest_tick(const std::string& symbol, MarketTick* out) const;
    std::string latest_tick_json(const std::string& symbol) const;
    std::string health_json() const;

    bool authorize_request(
        const std::string& provided_token,
        GatewayRejectReason* reason = nullptr,
        bool count_as_order_request = true);
    bool add_token(const std::string& token, uint64_t ttl_ms = 0);
    bool revoke_token(const std::string& token);
    bool rotate_token(const std::string& old_token, const std::string& new_token, uint64_t ttl_ms = 0);
    void record_order_result(bool accepted);
    void emit_gateway_reject_event(
        uint64_t order_id,
        const Order& order,
        GatewayRejectReason reason,
        const std::string& token);
    GatewayMetrics metrics() const;
    void reset_metrics();

private:
    struct RateWindowState {
        std::chrono::steady_clock::time_point window_start{};
        uint32_t requests = 0;
    };

    bool consume_rate_limit(const std::string& key);
    bool token_allowed_unlocked(const std::string& token, uint64_t now_ns);
    void on_market_message(const void* data, size_t size);
    static std::string normalize_key(const char* symbol);

    std::shared_ptr<bus::MessageBus> bus_;
    std::shared_ptr<persist::EventJournal> journal_;
    std::string market_topic_;
    GatewaySecurityConfig security_;
    std::atomic<bool> started_{false};

    mutable std::mutex mutex_;
    std::unordered_map<std::string, MarketTick> latest_ticks_;
    std::unordered_map<std::string, uint64_t> token_expiry_ns_;
    std::unordered_map<std::string, RateWindowState> rate_windows_;
    std::atomic<uint64_t> ticks_received_{0};
    std::atomic<uint64_t> ticks_decoded_{0};
    std::atomic<uint64_t> decode_errors_{0};
    std::atomic<uint64_t> order_requests_{0};
    std::atomic<uint64_t> order_accepted_{0};
    std::atomic<uint64_t> order_rejected_{0};
    std::atomic<uint64_t> auth_failures_{0};
    std::atomic<uint64_t> rate_limited_{0};
};

OrderAck submit_order(trading::OrderManager& manager, const Order& order);
OrderAck submit_order(
    MarketGatewayService& gateway,
    trading::OrderManager& manager,
    const Order& order,
    const std::string& api_token);

const char* reject_reason_to_string(trading::OrderRejectReason reason);
const char* gateway_reject_reason_to_string(GatewayRejectReason reason);
std::string to_json(const MarketTick& tick);
std::string to_json(const OrderAck& ack);
std::string to_json(const GatewayMetrics& metrics, bool running);

} // namespace argentum::api
