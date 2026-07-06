#pragma once

#include "api/market_gateway.hpp"
#include "bus/message_bus.hpp"
#include "trading/order_manager.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace argentum::api {

struct HttpWsServerConfig {
    uint32_t max_requests_per_ip = 600;
    uint32_t ip_window_ms = 1000;
    bool audit_access = true;
};

class HttpWsServer {
public:
    HttpWsServer(
        std::shared_ptr<bus::MessageBus> bus,
        MarketGatewayService& gateway,
        trading::OrderManager& order_manager,
        uint16_t port = 8080,
        std::string market_topic = "market.ticks",
        HttpWsServerConfig config = {});

    ~HttpWsServer();

    bool start();
    void stop();
    bool is_running() const;

private:
    struct ConnectionState {
        bool is_websocket = false;
        std::string buffer;
        std::string client_id;
    };

    void accept_loop();
    void client_loop(intptr_t fd);

    void handle_http_buffer(intptr_t fd);
    void handle_websocket_buffer(intptr_t fd);
    void handle_http_request(intptr_t fd, const std::string& request);

    void send_http_response(
        intptr_t fd,
        int status_code,
        const std::string& content_type,
        const std::string& body,
        const std::string& extra_headers = "");
    void send_ws_text(intptr_t fd, const std::string& payload);
    void send_ws_pong(intptr_t fd, const std::string& payload);
    void close_socket(intptr_t fd);

    void on_market_tick(const void* data, size_t size);
    void broadcast_json_event(const std::string& payload);

    bool parse_order_json(const std::string& body, Order* out_order) const;
    static std::string extract_header(const std::string& headers, const std::string& key);
    static std::string extract_bearer_token(const std::string& headers);
    static std::string websocket_accept_key(const std::string& client_key);
    static std::string decode_url_component(const std::string& value);
    static std::string get_query_param(const std::string& target, const std::string& key);
    static std::string to_lower(std::string value);
    static std::string reason_phrase(int status_code);
    static std::string path_from_target(const std::string& target);
    static std::string openmetrics_from_gateway(const GatewayMetrics& metrics, size_t active_orders);

    bool allow_ip_request(const std::string& ip);
    static std::string peer_ip(intptr_t fd);
    void audit_access(const std::string& ip, const std::string& method, const std::string& path, int status_code) const;

    std::shared_ptr<bus::MessageBus> bus_;
    MarketGatewayService& gateway_;
    trading::OrderManager& order_manager_;
    uint16_t port_;
    std::string market_topic_;
    HttpWsServerConfig config_;
    intptr_t listen_fd_ = -1;
    std::thread accept_thread_;
    std::vector<std::thread> client_threads_;
    std::atomic<bool> running_{false};

    struct IpRateState {
        std::chrono::steady_clock::time_point window_start{};
        uint32_t requests = 0;
    };

    mutable std::mutex mutex_;
    std::unordered_map<intptr_t, ConnectionState> connections_;
    std::unordered_map<std::string, IpRateState> ip_rate_windows_;
};

} // namespace argentum::api
