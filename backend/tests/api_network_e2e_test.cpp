#include "api/http_ws_server.hpp"
#include "api/market_gateway.hpp"
#include "bus/message_bus.hpp"
#include "codec/market_tick_codec.hpp"
#include "engine/order_book.hpp"
#include "risk/risk_manager.hpp"
#include "trading/order_manager.hpp"

#include <chrono>
#include <array>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#error "api_network_e2e_test currently supports Windows only."
#endif

namespace {

#define REQUIRE(cond, msg) \
    do { \
        if (!(cond)) { \
            std::cerr << "[api_network_e2e_test] " << (msg) << std::endl; \
            return 1; \
        } \
    } while (0)

SOCKET connect_local(uint16_t port) {
    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) return INVALID_SOCKET;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) != 1) {
        closesocket(sock);
        return INVALID_SOCKET;
    }

    u_long non_blocking = 1;
    if (ioctlsocket(sock, FIONBIO, &non_blocking) != 0) {
        closesocket(sock);
        return INVALID_SOCKET;
    }

    int rc = connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if (rc != 0) {
        const int wsa_err = WSAGetLastError();
        if (wsa_err != WSAEWOULDBLOCK && wsa_err != WSAEINPROGRESS) {
            closesocket(sock);
            return INVALID_SOCKET;
        }

        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(sock, &wfds);
        timeval tv{};
        tv.tv_sec = 0;
        tv.tv_usec = 500 * 1000; // 500ms
        rc = select(0, nullptr, &wfds, nullptr, &tv);
        if (rc <= 0) {
            closesocket(sock);
            return INVALID_SOCKET;
        }
        int so_error = 0;
        int so_len = sizeof(so_error);
        if (getsockopt(sock, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&so_error), &so_len) != 0 ||
            so_error != 0) {
            closesocket(sock);
            return INVALID_SOCKET;
        }
    }

    non_blocking = 0;
    (void)ioctlsocket(sock, FIONBIO, &non_blocking);

    const int timeout_ms = 3000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
    return sock;
}

std::string http_roundtrip(uint16_t port, const std::string& request) {
    SOCKET sock = connect_local(port);
    if (sock == INVALID_SOCKET) {
        return "";
    }

    if (send(sock, request.data(), static_cast<int>(request.size()), 0) !=
        static_cast<int>(request.size())) {
        closesocket(sock);
        return "";
    }

    std::string response;
    std::array<char, 2048> buffer{};
    for (;;) {
        const int received = recv(sock, buffer.data(), static_cast<int>(buffer.size()), 0);
        if (received <= 0) break;
        response.append(buffer.data(), static_cast<size_t>(received));
    }
    closesocket(sock);
    return response;
}

bool recv_exact(SOCKET sock, void* buffer, size_t size) {
    uint8_t* ptr = static_cast<uint8_t*>(buffer);
    size_t done = 0;
    while (done < size) {
        const int rc = recv(sock, reinterpret_cast<char*>(ptr + done), static_cast<int>(size - done), 0);
        if (rc <= 0) return false;
        done += static_cast<size_t>(rc);
    }
    return true;
}

bool recv_ws_text(SOCKET sock, std::string* payload) {
    if (!payload) return false;
    std::array<uint8_t, 2> header{};
    if (!recv_exact(sock, header.data(), header.size())) return false;

    const uint8_t opcode = static_cast<uint8_t>(header[0] & 0x0F);
    if (opcode != 0x1) return false;

    uint64_t length = static_cast<uint64_t>(header[1] & 0x7F);
    if (length == 126) {
        std::array<uint8_t, 2> ext{};
        if (!recv_exact(sock, ext.data(), ext.size())) return false;
        length = (static_cast<uint64_t>(ext[0]) << 8U) | ext[1];
    } else if (length == 127) {
        std::array<uint8_t, 8> ext{};
        if (!recv_exact(sock, ext.data(), ext.size())) return false;
        length = 0;
        for (uint8_t b : ext) {
            length = (length << 8U) | b;
        }
    }

    std::vector<char> data(static_cast<size_t>(length));
    if (length > 0 && !recv_exact(sock, data.data(), static_cast<size_t>(length))) return false;
    payload->assign(data.begin(), data.end());
    return true;
}

} // namespace

int main() {
    const char* enabled = std::getenv("ARGENTUM_RUN_NETWORK_E2E");
    if (!enabled || std::string(enabled) != "1") {
        return 0;
    }

    WSADATA wsa{};
    REQUIRE(WSAStartup(MAKEWORD(2, 2), &wsa) == 0, "WSAStartup failed");

    argentum::bus::InprocBusConfig bus_cfg{};
    bus_cfg.consumer_threads = 1;
    bus_cfg.queue_capacity = 256;
    auto bus = argentum::bus::create_inproc_bus(bus_cfg);
    bus->connect("inproc://network-e2e", true);

    argentum::api::GatewaySecurityConfig security{};
    security.api_token = "integration-token";
    security.rate_limit.max_requests = 1000;
    security.rate_limit.window_ms = 1000;
    argentum::api::MarketGatewayService gateway(bus, "market.ticks", security);
    gateway.start();

    auto book = std::make_shared<argentum::engine::OrderBook>("BTC/USDT");
    auto risk = std::make_shared<argentum::risk::RiskManager>(argentum::risk::RiskLimits{
        1'000'000.0,
        1'000'000.0,
        1'000'000.0
    });
    argentum::trading::OrderManager oms(risk, book);

    const uint16_t base_port = 19080;
    std::unique_ptr<argentum::api::HttpWsServer> server;
    uint16_t selected_port = 0;
    for (uint16_t port = base_port; port < static_cast<uint16_t>(base_port + 20); ++port) {
        server = std::make_unique<argentum::api::HttpWsServer>(bus, gateway, oms, port);
        if (server->start()) {
            selected_port = port;
            break;
        }
        server.reset();
    }
    REQUIRE(server != nullptr, "No free TCP port found for HTTP/WS server");
    REQUIRE(selected_port != 0, "Invalid selected port");

    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    const std::string health_request =
        "GET /api/v1/health HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Connection: close\r\n\r\n";
    std::string health_response;
    for (int i = 0; i < 20; ++i) {
        health_response = http_roundtrip(selected_port, health_request);
        if (health_response.find("HTTP/1.1") != std::string::npos) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (health_response.find("200 OK") == std::string::npos) {
        std::cerr << "[api_network_e2e_test] Health raw response: " << health_response << std::endl;
        REQUIRE(false, "Health endpoint did not return HTTP 200");
    }
    REQUIRE(health_response.find("\"status\":\"ok\"") != std::string::npos, "Health endpoint payload mismatch");

    const std::string order_body =
        "{\"order_id\":4001,\"client_id\":42,\"symbol\":\"BTC/USDT\",\"side\":\"buy\","
        "\"type\":\"limit\",\"price\":50000.0,\"quantity\":0.25}";
    const std::string order_request =
        "POST /api/v1/orders HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Authorization: Bearer integration-token\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: " + std::to_string(order_body.size()) + "\r\n"
        "Connection: close\r\n\r\n" + order_body;
    const std::string order_response = http_roundtrip(selected_port, order_request);
    REQUIRE(order_response.find("200 OK") != std::string::npos, "Order endpoint did not return HTTP 200");
    REQUIRE(order_response.find("\"accepted\":true") != std::string::npos, "Order was not accepted");

    SOCKET ws = connect_local(selected_port);
    REQUIRE(ws != INVALID_SOCKET, "WS TCP connect failed");
    const std::string ws_handshake =
        "GET /ws?token=integration-token HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13\r\n\r\n";
    REQUIRE(send(ws, ws_handshake.data(), static_cast<int>(ws_handshake.size()), 0) ==
            static_cast<int>(ws_handshake.size()),
            "WS handshake send failed");

    std::array<char, 1024> handshake_response{};
    const int hs_size = recv(ws, handshake_response.data(), static_cast<int>(handshake_response.size()), 0);
    REQUIRE(hs_size > 0, "WS handshake receive failed");
    const std::string hs(handshake_response.data(), static_cast<size_t>(hs_size));
    REQUIRE(hs.find("101 Switching Protocols") != std::string::npos, "WS upgrade rejected");

    MarketTick tick{};
    tick.timestamp_ns = 1700000009999999999ULL;
    tick.price = 49999.5;
    tick.quantity = 0.10;
    tick.side = SIDE_BUY;
    std::strncpy(tick.symbol, "BTC/USDT", sizeof(tick.symbol) - 1);
    std::strncpy(tick.source, "BINANCE", sizeof(tick.source) - 1);

    std::vector<uint8_t> payload;
    REQUIRE(argentum::codec::encode_market_tick_legacy(tick, &payload) == ARGENTUM_OK, "Tick encoding failed");
    REQUIRE(bus->publish("market.ticks", payload.data(), payload.size()) == ARGENTUM_OK, "Tick publish failed");

    std::string ws_payload;
    REQUIRE(recv_ws_text(ws, &ws_payload), "WS tick frame was not received");
    REQUIRE(ws_payload.find("\"event\":\"tick\"") != std::string::npos, "WS event type mismatch");
    REQUIRE(ws_payload.find("\"symbol\":\"BTC/USDT\"") != std::string::npos, "WS symbol mismatch");

    const std::string metrics_request =
        "GET /metrics HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Connection: close\r\n\r\n";
    const std::string metrics_response = http_roundtrip(selected_port, metrics_request);
    REQUIRE(metrics_response.find("200 OK") != std::string::npos, "Metrics endpoint did not return HTTP 200");
    REQUIRE(metrics_response.find("argentum_order_requests_total") != std::string::npos,
            "Metrics payload missing expected series");

    closesocket(ws);
    server->stop();
    gateway.stop();
    WSACleanup();
    return 0;
}
