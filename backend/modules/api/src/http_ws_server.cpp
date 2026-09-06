#include "api/http_ws_server.hpp"

#include "audit/logger.hpp"
#include "codec/market_tick_codec.hpp"
#include "core/fixed_point.hpp"
#include "core/time_utils.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <string_view>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#error "HttpWsServer currently supports Windows sockets."
#endif

namespace argentum::api {

namespace {
constexpr const char* kWebSocketGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

std::array<uint8_t, 20> sha1_bytes(const std::string& input) {
    auto left_rotate = [](uint32_t value, size_t bits) -> uint32_t {
        return (value << bits) | (value >> (32U - bits));
    };

    std::vector<uint8_t> data(input.begin(), input.end());
    const uint64_t bit_len = static_cast<uint64_t>(data.size()) * 8ULL;
    data.push_back(0x80);
    while ((data.size() % 64) != 56) {
        data.push_back(0);
    }
    for (int i = 7; i >= 0; --i) {
        data.push_back(static_cast<uint8_t>((bit_len >> (i * 8)) & 0xFF));
    }

    uint32_t h0 = 0x67452301;
    uint32_t h1 = 0xEFCDAB89;
    uint32_t h2 = 0x98BADCFE;
    uint32_t h3 = 0x10325476;
    uint32_t h4 = 0xC3D2E1F0;

    for (size_t chunk = 0; chunk < data.size(); chunk += 64) {
        uint32_t w[80] = {0};
        for (int i = 0; i < 16; ++i) {
            const size_t idx = chunk + static_cast<size_t>(i) * 4;
            w[i] = (static_cast<uint32_t>(data[idx]) << 24U) |
                   (static_cast<uint32_t>(data[idx + 1]) << 16U) |
                   (static_cast<uint32_t>(data[idx + 2]) << 8U) |
                   static_cast<uint32_t>(data[idx + 3]);
        }
        for (int i = 16; i < 80; ++i) {
            w[i] = left_rotate(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        }

        uint32_t a = h0;
        uint32_t b = h1;
        uint32_t c = h2;
        uint32_t d = h3;
        uint32_t e = h4;

        for (int i = 0; i < 80; ++i) {
            uint32_t f = 0;
            uint32_t k = 0;
            if (i < 20) {
                f = (b & c) | ((~b) & d);
                k = 0x5A827999;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDC;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6;
            }

            const uint32_t temp = left_rotate(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = left_rotate(b, 30);
            b = a;
            a = temp;
        }

        h0 += a;
        h1 += b;
        h2 += c;
        h3 += d;
        h4 += e;
    }

    std::array<uint8_t, 20> out{};
    const uint32_t hs[5] = {h0, h1, h2, h3, h4};
    for (size_t i = 0; i < 5; ++i) {
        out[i * 4] = static_cast<uint8_t>((hs[i] >> 24U) & 0xFF);
        out[i * 4 + 1] = static_cast<uint8_t>((hs[i] >> 16U) & 0xFF);
        out[i * 4 + 2] = static_cast<uint8_t>((hs[i] >> 8U) & 0xFF);
        out[i * 4 + 3] = static_cast<uint8_t>(hs[i] & 0xFF);
    }
    return out;
}

std::string base64_encode(const uint8_t* data, size_t len) {
    static constexpr char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((len + 2) / 3) * 4);

    for (size_t i = 0; i < len; i += 3) {
        const uint32_t octet_a = data[i];
        const uint32_t octet_b = (i + 1 < len) ? data[i + 1] : 0;
        const uint32_t octet_c = (i + 2 < len) ? data[i + 2] : 0;
        const uint32_t triple = (octet_a << 16U) | (octet_b << 8U) | octet_c;

        out.push_back(kAlphabet[(triple >> 18U) & 0x3F]);
        out.push_back(kAlphabet[(triple >> 12U) & 0x3F]);
        out.push_back((i + 1 < len) ? kAlphabet[(triple >> 6U) & 0x3F] : '=');
        out.push_back((i + 2 < len) ? kAlphabet[triple & 0x3F] : '=');
    }
    return out;
}

bool parse_double_field(const std::string& json, const std::string& key, double* out) {
    if (!out) return false;
    const std::string marker = "\"" + key + "\"";
    size_t pos = json.find(marker);
    if (pos == std::string::npos) return false;
    pos = json.find(':', pos + marker.size());
    if (pos == std::string::npos) return false;
    ++pos;
    while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos])) != 0) ++pos;
    size_t end = pos;
    while (end < json.size() &&
           (std::isdigit(static_cast<unsigned char>(json[end])) != 0 ||
            json[end] == '.' || json[end] == '-' || json[end] == '+' ||
            json[end] == 'e' || json[end] == 'E')) {
        ++end;
    }
    if (end == pos) return false;
    *out = std::strtod(json.substr(pos, end - pos).c_str(), nullptr);
    return true;
}

bool parse_u64_field(const std::string& json, const std::string& key, uint64_t* out) {
    if (!out) return false;
    const std::string marker = "\"" + key + "\"";
    size_t pos = json.find(marker);
    if (pos == std::string::npos) return false;
    pos = json.find(':', pos + marker.size());
    if (pos == std::string::npos) return false;
    ++pos;
    while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos])) != 0) ++pos;
    size_t end = pos;
    while (end < json.size() && std::isdigit(static_cast<unsigned char>(json[end])) != 0) ++end;
    if (end == pos) return false;
    *out = static_cast<uint64_t>(std::strtoull(json.substr(pos, end - pos).c_str(), nullptr, 10));
    return true;
}

bool parse_string_field(const std::string& json, const std::string& key, std::string* out) {
    if (!out) return false;
    const std::string marker = "\"" + key + "\"";
    size_t pos = json.find(marker);
    if (pos == std::string::npos) return false;
    pos = json.find(':', pos + marker.size());
    if (pos == std::string::npos) return false;
    pos = json.find('"', pos + 1);
    if (pos == std::string::npos) return false;
    const size_t end = json.find('"', pos + 1);
    if (end == std::string::npos) return false;
    *out = json.substr(pos + 1, end - pos - 1);
    return true;
}

bool send_all(intptr_t fd, const char* data, size_t size, int timeout_ms = 1000) {
    if (!data || size == 0) return true;
    SOCKET sock = static_cast<SOCKET>(fd);
    size_t total = 0;
    while (total < size) {
        const int sent = send(sock, data + total, static_cast<int>(size - total), 0);
        if (sent > 0) {
            total += static_cast<size_t>(sent);
            continue;
        }
        if (sent == 0) {
            return false;
        }
        const int err = WSAGetLastError();
        if (err == WSAEWOULDBLOCK) {
            fd_set wfds;
            FD_ZERO(&wfds);
            FD_SET(sock, &wfds);
            timeval tv{};
            tv.tv_sec = timeout_ms / 1000;
            tv.tv_usec = (timeout_ms % 1000) * 1000;
            const int ready = select(0, nullptr, &wfds, nullptr, &tv);
            if (ready <= 0) {
                return false;
            }
            continue;
        }
        return false;
    }
    return true;
}

} // namespace

HttpWsServer::HttpWsServer(
    std::shared_ptr<bus::MessageBus> bus,
    MarketGatewayService& gateway,
    trading::OrderManager& order_manager,
    uint16_t port,
    std::string market_topic,
    HttpWsServerConfig config)
    : bus_(std::move(bus)),
      gateway_(gateway),
      order_manager_(order_manager),
      port_(port),
      market_topic_(std::move(market_topic)),
      config_(config) {}

HttpWsServer::~HttpWsServer() {
    stop();
}

bool HttpWsServer::start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true, std::memory_order_relaxed)) {
        return true;
    }

    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) {
        running_.store(false, std::memory_order_relaxed);
        return false;
    }

    int exclusive = 1;
#ifdef SO_EXCLUSIVEADDRUSE
    setsockopt(listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
#endif
    u_long non_blocking = 1;
    (void)ioctlsocket(listener, FIONBIO, &non_blocking);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port_);

    if (bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        closesocket(listener);
        running_.store(false, std::memory_order_relaxed);
        return false;
    }
    if (listen(listener, SOMAXCONN) != 0) {
        closesocket(listener);
        running_.store(false, std::memory_order_relaxed);
        return false;
    }
    listen_fd_ = static_cast<intptr_t>(listener);

    if (bus_) {
        bus_->subscribe(market_topic_, [this](const void* data, size_t size) { on_market_tick(data, size); });
    }

    accept_thread_ = std::thread([this] { accept_loop(); });
    return true;
}

void HttpWsServer::stop() {
    if (!running_.exchange(false, std::memory_order_relaxed)) return;

    if (listen_fd_ != -1) {
        closesocket(static_cast<SOCKET>(listen_fd_));
        listen_fd_ = -1;
    }
    if (accept_thread_.joinable()) {
        accept_thread_.join();
    }

    std::vector<intptr_t> fds;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        fds.reserve(connections_.size());
        for (const auto& [fd, _] : connections_) {
            fds.push_back(fd);
        }
    }
    for (intptr_t fd : fds) {
        close_socket(fd);
    }

    std::vector<std::thread> threads;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        threads.swap(client_threads_);
    }
    for (auto& thread : threads) {
        if (thread.joinable()) {
            thread.join();
        }
    }

    std::lock_guard<std::mutex> lock(mutex_);
    connections_.clear();
    ip_rate_windows_.clear();
}

bool HttpWsServer::is_running() const {
    return running_.load(std::memory_order_relaxed);
}

void HttpWsServer::accept_loop() {
    const SOCKET listener = static_cast<SOCKET>(listen_fd_);
    while (running_.load(std::memory_order_relaxed)) {
        SOCKET client = accept(listener, nullptr, nullptr);
        if (client == INVALID_SOCKET) {
            const int err = WSAGetLastError();
            if (err == WSAEWOULDBLOCK) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }
            if (running_.load(std::memory_order_relaxed)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }
            break;
        }

        const int timeout_ms = 1000;
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
        setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));

        const intptr_t fd = static_cast<intptr_t>(client);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            connections_[fd] = ConnectionState{};
        }
        std::thread worker([this, fd] { client_loop(fd); });
        std::lock_guard<std::mutex> lock(mutex_);
        client_threads_.push_back(std::move(worker));
    }
}

void HttpWsServer::client_loop(intptr_t fd) {
    std::array<char, 4096> buffer{};
    while (running_.load(std::memory_order_relaxed)) {
        const int received = recv(static_cast<SOCKET>(fd), buffer.data(), static_cast<int>(buffer.size()), 0);
        if (received == 0) {
            break;
        }
        if (received < 0) {
            const int err = WSAGetLastError();
            if (err == WSAETIMEDOUT || err == WSAEWOULDBLOCK) {
                continue;
            }
            break;
        }

        bool is_websocket = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = connections_.find(fd);
            if (it == connections_.end()) break;
            it->second.buffer.append(buffer.data(), static_cast<size_t>(received));
            is_websocket = it->second.is_websocket;
        }

        if (is_websocket) {
            handle_websocket_buffer(fd);
        } else {
            handle_http_buffer(fd);
        }

        std::lock_guard<std::mutex> lock(mutex_);
        if (connections_.find(fd) == connections_.end()) {
            break;
        }
    }
    close_socket(fd);
}

void HttpWsServer::handle_http_buffer(intptr_t fd) {
    for (;;) {
        std::string request;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = connections_.find(fd);
            if (it == connections_.end()) return;
            ConnectionState& state = it->second;
            const size_t header_end = state.buffer.find("\r\n\r\n");
            if (header_end == std::string::npos) return;

            const std::string headers = state.buffer.substr(0, header_end + 4);
            size_t content_length = 0;
            const std::string content_len_header = extract_header(headers, "content-length");
            if (!content_len_header.empty()) {
                content_length = static_cast<size_t>(std::strtoull(content_len_header.c_str(), nullptr, 10));
            }
            if (state.buffer.size() < header_end + 4 + content_length) return;
            request = state.buffer.substr(0, header_end + 4 + content_length);
            state.buffer.erase(0, header_end + 4 + content_length);
        }
        handle_http_request(fd, request);
    }
}

void HttpWsServer::handle_http_request(intptr_t fd, const std::string& request) {
    const std::string ip = peer_ip(fd);
    const size_t line_end = request.find("\r\n");
    if (line_end == std::string::npos) {
        send_http_response(fd, 400, "application/json", "{\"error\":\"bad_request\"}");
        audit_access(ip, "?", "?", 400);
        close_socket(fd);
        return;
    }

    const std::string request_line = request.substr(0, line_end);
    std::istringstream line_stream(request_line);
    std::string method;
    std::string target;
    std::string version;
    line_stream >> method >> target >> version;
    if (method.empty() || target.empty()) {
        send_http_response(fd, 400, "application/json", "{\"error\":\"bad_request\"}");
        audit_access(ip, method, target, 400);
        close_socket(fd);
        return;
    }

    const size_t header_end = request.find("\r\n\r\n");
    const std::string headers = (header_end == std::string::npos) ? "" : request.substr(0, header_end + 2);
    const std::string body = (header_end == std::string::npos) ? "" : request.substr(header_end + 4);
    const std::string method_lc = to_lower(method);
    const std::string path = path_from_target(target);
    const std::string upgrade = to_lower(extract_header(headers, "upgrade"));

    if (method_lc != "get" || path != "/metrics") {
        if (!allow_ip_request(ip)) {
            send_http_response(fd, 429, "application/json", "{\"error\":\"ip_rate_limited\"}");
            audit_access(ip, method, path, 429);
            close_socket(fd);
            return;
        }
    }

    if (method_lc == "get" && path == "/ws" && upgrade == "websocket") {
        const std::string ws_key = extract_header(headers, "sec-websocket-key");
        if (ws_key.empty()) {
            send_http_response(fd, 400, "application/json", "{\"error\":\"missing_websocket_key\"}");
            audit_access(ip, method, path, 400);
            close_socket(fd);
            return;
        }

        const std::string token = get_query_param(target, "token");
        GatewayRejectReason reject = GatewayRejectReason::None;
        if (!gateway_.authorize_request(token, &reject, false)) {
            const int status = (reject == GatewayRejectReason::RateLimited) ? 429 : 401;
            send_http_response(fd, status, "application/json", "{\"error\":\"unauthorized\"}");
            audit_access(ip, method, path, status);
            close_socket(fd);
            return;
        }

        std::ostringstream response;
        response << "HTTP/1.1 101 Switching Protocols\r\n"
                 << "Upgrade: websocket\r\n"
                 << "Connection: Upgrade\r\n"
                 << "Sec-WebSocket-Accept: " << websocket_accept_key(ws_key) << "\r\n\r\n";
        const std::string payload = response.str();
        if (!send_all(fd, payload.data(), payload.size())) {
            close_socket(fd);
            return;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = connections_.find(fd);
            if (it != connections_.end()) {
                it->second.is_websocket = true;
                it->second.client_id = token.empty() ? "anonymous" : token;
                it->second.buffer.clear();
            }
        }
        audit_access(ip, method, path, 101);
        return;
    }

    if (method_lc == "get" && path == "/api/v1/health") {
        send_http_response(fd, 200, "application/json", gateway_.health_json());
        audit_access(ip, method, path, 200);
        close_socket(fd);
        return;
    }

    if (method_lc == "get" && path == "/metrics") {
        const auto metrics = gateway_.metrics();
        send_http_response(fd, 200, "text/plain; version=0.0.4",
                           openmetrics_from_gateway(metrics, order_manager_.active_order_count()));
        audit_access(ip, method, path, 200);
        close_socket(fd);
        return;
    }

    if (method_lc == "get" && path.rfind("/api/v1/markets/", 0) == 0 &&
        path.size() > std::strlen("/api/v1/markets/")) {
        const std::string suffix = "/snapshot";
        if (path.size() > suffix.size() && path.rfind(suffix) == path.size() - suffix.size()) {
            const size_t begin = std::strlen("/api/v1/markets/");
            const std::string encoded_symbol = path.substr(begin, path.size() - begin - suffix.size());
            const std::string symbol = decode_url_component(encoded_symbol);
            send_http_response(fd, 200, "application/json", gateway_.latest_tick_json(symbol));
            audit_access(ip, method, path, 200);
            close_socket(fd);
            return;
        }
    }

    if (method_lc == "post" && path == "/api/v1/orders") {
        Order order{};
        if (!parse_order_json(body, &order)) {
            send_http_response(fd, 422, "application/json", "{\"error\":\"invalid_order_payload\"}");
            audit_access(ip, method, path, 422);
            close_socket(fd);
            return;
        }
        const std::string token = extract_bearer_token(headers);
        OrderAck ack = submit_order(gateway_, order_manager_, order, token);
        int status = 200;
        if (ack.gateway_reject_reason == GatewayRejectReason::Unauthorized) {
            status = 401;
        } else if (ack.gateway_reject_reason == GatewayRejectReason::RateLimited) {
            status = 429;
        } else {
            status = ack.accepted ? 200 : 422;
        }
        send_http_response(fd, status, "application/json", to_json(ack));
        audit_access(ip, method, path, status);
        close_socket(fd);
        return;
    }

    send_http_response(fd, 404, "application/json", "{\"error\":\"not_found\"}");
    audit_access(ip, method, path, 404);
    close_socket(fd);
}

void HttpWsServer::handle_websocket_buffer(intptr_t fd) {
    for (;;) {
        uint8_t opcode = 0;
        std::string payload;
        bool have_frame = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = connections_.find(fd);
            if (it == connections_.end()) return;
            ConnectionState& state = it->second;
            if (state.buffer.size() < 2) return;

            const uint8_t b0 = static_cast<uint8_t>(state.buffer[0]);
            const uint8_t b1 = static_cast<uint8_t>(state.buffer[1]);
            opcode = static_cast<uint8_t>(b0 & 0x0F);
            const bool masked = (b1 & 0x80U) != 0;

            uint64_t payload_len = static_cast<uint64_t>(b1 & 0x7FU);
            size_t offset = 2;
            if (payload_len == 126) {
                if (state.buffer.size() < offset + 2) return;
                payload_len = (static_cast<uint8_t>(state.buffer[offset]) << 8U) |
                              static_cast<uint8_t>(state.buffer[offset + 1]);
                offset += 2;
            } else if (payload_len == 127) {
                if (state.buffer.size() < offset + 8) return;
                payload_len = 0;
                for (int i = 0; i < 8; ++i) {
                    payload_len = (payload_len << 8U) | static_cast<uint8_t>(state.buffer[offset + i]);
                }
                offset += 8;
            }

            std::array<uint8_t, 4> mask = {0, 0, 0, 0};
            if (masked) {
                if (state.buffer.size() < offset + 4) return;
                for (size_t i = 0; i < 4; ++i) {
                    mask[i] = static_cast<uint8_t>(state.buffer[offset + i]);
                }
                offset += 4;
            }
            if (state.buffer.size() < offset + payload_len) return;

            payload = state.buffer.substr(offset, static_cast<size_t>(payload_len));
            state.buffer.erase(0, offset + static_cast<size_t>(payload_len));
            if (masked) {
                for (size_t i = 0; i < payload.size(); ++i) {
                    payload[i] = static_cast<char>(static_cast<uint8_t>(payload[i]) ^ mask[i % 4]);
                }
            }
            have_frame = true;
        }

        if (!have_frame) return;
        if (opcode == 0x8) { // close
            close_socket(fd);
            return;
        }
        if (opcode == 0x9) { // ping
            send_ws_pong(fd, payload);
            continue;
        }
    }
}

void HttpWsServer::send_http_response(
    intptr_t fd,
    int status_code,
    const std::string& content_type,
    const std::string& body,
    const std::string& extra_headers) {
    std::ostringstream response;
    response << "HTTP/1.1 " << status_code << " " << reason_phrase(status_code) << "\r\n"
             << "Content-Type: " << content_type << "\r\n"
             << "Content-Length: " << body.size() << "\r\n"
             << "Connection: close\r\n";
    if (!extra_headers.empty()) {
        response << extra_headers;
        if (extra_headers.rfind("\r\n", extra_headers.size() - 1) == std::string::npos) {
            response << "\r\n";
        }
    }
    response << "\r\n" << body;
    const std::string payload = response.str();
    (void)send_all(fd, payload.data(), payload.size());
}

void HttpWsServer::send_ws_text(intptr_t fd, const std::string& payload) {
    std::string frame;
    frame.push_back(static_cast<char>(0x81)); // FIN + text

    const size_t len = payload.size();
    if (len < 126) {
        frame.push_back(static_cast<char>(len));
    } else if (len <= 0xFFFF) {
        frame.push_back(static_cast<char>(126));
        frame.push_back(static_cast<char>((len >> 8U) & 0xFF));
        frame.push_back(static_cast<char>(len & 0xFF));
    } else {
        frame.push_back(static_cast<char>(127));
        for (int i = 7; i >= 0; --i) {
            frame.push_back(static_cast<char>((len >> (i * 8)) & 0xFF));
        }
    }
    frame += payload;
    (void)send_all(fd, frame.data(), frame.size());
}

void HttpWsServer::send_ws_pong(intptr_t fd, const std::string& payload) {
    std::string frame;
    frame.push_back(static_cast<char>(0x8A)); // FIN + pong
    frame.push_back(static_cast<char>(payload.size()));
    frame += payload;
    (void)send_all(fd, frame.data(), frame.size());
}

void HttpWsServer::close_socket(intptr_t fd) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        connections_.erase(fd);
    }
    closesocket(static_cast<SOCKET>(fd));
}

void HttpWsServer::on_market_tick(const void* data, size_t size) {
    MarketTick tick{};
    if (codec::decode_market_tick(data, size, &tick) != ARGENTUM_OK) return;
    broadcast_json_event(to_json(tick));
}

void HttpWsServer::broadcast_json_event(const std::string& payload) {
    std::vector<intptr_t> ws_clients;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ws_clients.reserve(connections_.size());
        for (const auto& [fd, state] : connections_) {
            if (state.is_websocket) {
                ws_clients.push_back(fd);
            }
        }
    }

    for (intptr_t fd : ws_clients) {
        send_ws_text(fd, payload);
    }
}

bool HttpWsServer::parse_order_json(const std::string& body, Order* out_order) const {
    if (!out_order) return false;
    Order order{};
    std::string symbol;
    std::string side;
    std::string type;
    std::string tif = "gtc";
    double price = 0.0;
    double quantity = 0.0;

    (void)parse_u64_field(body, "order_id", &order.order_id);
    (void)parse_u64_field(body, "client_id", &order.client_id);
    if (!parse_string_field(body, "symbol", &symbol)) return false;
    if (!parse_string_field(body, "side", &side)) return false;
    if (!parse_string_field(body, "type", &type)) return false;
    (void)parse_string_field(body, "time_in_force", &tif);
    if (!parse_double_field(body, "quantity", &quantity)) return false;
    const bool has_price = parse_double_field(body, "price", &price);
    if (!has_price) {
        price = 0.0;
    }

    order.timestamp_ns = core::unix_now_ns();
    order.price = price;
    order.quantity = quantity;
    order.price_ticks = core::to_price_ticks(price);
    order.quantity_lots = core::to_quantity_lots(quantity);
    std::strncpy(order.symbol, symbol.c_str(), sizeof(order.symbol) - 1);

    const std::string side_lc = to_lower(side);
    if (side_lc == "buy" || side_lc == "b") {
        order.side = SIDE_BUY;
    } else if (side_lc == "sell" || side_lc == "s") {
        order.side = SIDE_SELL;
    } else {
        return false;
    }

    const std::string type_lc = to_lower(type);
    if (type_lc == "limit") {
        order.type = ORDER_TYPE_LIMIT;
    } else if (type_lc == "market") {
        order.type = ORDER_TYPE_MARKET;
    } else if (type_lc == "stop") {
        order.type = ORDER_TYPE_STOP;
    } else {
        return false;
    }

    const std::string tif_lc = to_lower(tif);
    if (tif_lc == "gtc") {
        order.tif = TIF_GTC;
    } else if (tif_lc == "ioc") {
        order.tif = TIF_IOC;
    } else if (tif_lc == "fok") {
        order.tif = TIF_FOK;
    } else {
        return false;
    }

    if (quantity <= 0.0) return false;
    if (price <= 0.0) return false;

    *out_order = order;
    return true;
}

std::string HttpWsServer::extract_header(const std::string& headers, const std::string& key) {
    const std::string key_lc = to_lower(key);
    std::istringstream stream(headers);
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t sep = line.find(':');
        if (sep == std::string::npos) continue;
        std::string line_key = to_lower(line.substr(0, sep));
        if (line_key != key_lc) continue;
        std::string value = line.substr(sep + 1);
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())) != 0) {
            value.erase(value.begin());
        }
        return value;
    }
    return "";
}

std::string HttpWsServer::extract_bearer_token(const std::string& headers) {
    const std::string auth = extract_header(headers, "authorization");
    if (auth.size() < 7) return "";
    const std::string prefix = to_lower(auth.substr(0, 7));
    if (prefix != "bearer ") return "";
    return auth.substr(7);
}

std::string HttpWsServer::websocket_accept_key(const std::string& client_key) {
    const auto digest = sha1_bytes(client_key + kWebSocketGuid);
    return base64_encode(digest.data(), digest.size());
}

std::string HttpWsServer::decode_url_component(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '%' && i + 2 < value.size()) {
            const std::string hex = value.substr(i + 1, 2);
            out.push_back(static_cast<char>(std::strtol(hex.c_str(), nullptr, 16)));
            i += 2;
        } else if (value[i] == '+') {
            out.push_back(' ');
        } else {
            out.push_back(value[i]);
        }
    }
    return out;
}

std::string HttpWsServer::get_query_param(const std::string& target, const std::string& key) {
    const size_t q = target.find('?');
    if (q == std::string::npos) return "";
    const std::string query = target.substr(q + 1);
    std::istringstream ss(query);
    std::string pair;
    while (std::getline(ss, pair, '&')) {
        const size_t eq = pair.find('=');
        if (eq == std::string::npos) continue;
        if (pair.substr(0, eq) == key) {
            return decode_url_component(pair.substr(eq + 1));
        }
    }
    return "";
}

std::string HttpWsServer::to_lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::string HttpWsServer::path_from_target(const std::string& target) {
    const size_t q = target.find('?');
    if (q == std::string::npos) return target;
    return target.substr(0, q);
}

std::string HttpWsServer::reason_phrase(int status_code) {
    switch (status_code) {
        case 200: return "OK";
        case 101: return "Switching Protocols";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 422: return "Unprocessable Entity";
        case 429: return "Too Many Requests";
        default: return "Internal Server Error";
    }
}

bool HttpWsServer::allow_ip_request(const std::string& ip) {
    if (config_.max_requests_per_ip == 0) return true;
    const uint64_t now_ns = core::mono_now_ns();
    const uint64_t window_ns =
        static_cast<uint64_t>(config_.ip_window_ms == 0 ? 1 : config_.ip_window_ms) * 1'000'000ULL;
    std::lock_guard<std::mutex> lock(mutex_);
    auto& state = ip_rate_windows_[ip];
    if (state.window_start_mono_ns == 0) {
        state.window_start_mono_ns = now_ns;
    }
    if (now_ns - state.window_start_mono_ns >= window_ns) {
        state.window_start_mono_ns = now_ns;
        state.requests = 0;
    }
    if (state.requests >= config_.max_requests_per_ip) {
        return false;
    }
    ++state.requests;
    return true;
}

std::string HttpWsServer::peer_ip(intptr_t fd) {
    sockaddr_in addr{};
    int len = sizeof(addr);
    if (getpeername(static_cast<SOCKET>(fd), reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
        return "unknown";
    }
    char ip[64] = {0};
    if (!inet_ntop(AF_INET, &addr.sin_addr, ip, static_cast<DWORD>(sizeof(ip)))) {
        return "unknown";
    }
    return std::string(ip);
}

void HttpWsServer::audit_access(
    const std::string& ip,
    const std::string& method,
    const std::string& path,
    int status_code) const {
    if (!config_.audit_access) return;
    std::ostringstream message;
    message << "http_access ip=" << ip
            << " method=" << method
            << " path=" << path
            << " status=" << status_code;
    audit::Logger::instance().log(audit::LogLevel::AUDIT, message.str());
}

std::string HttpWsServer::openmetrics_from_gateway(const GatewayMetrics& metrics, size_t active_orders) {
    std::ostringstream os;
    os << "# TYPE argentum_ticks_received_total counter\n";
    os << "argentum_ticks_received_total " << metrics.ticks_received << "\n";
    os << "# TYPE argentum_ticks_decoded_total counter\n";
    os << "argentum_ticks_decoded_total " << metrics.ticks_decoded << "\n";
    os << "# TYPE argentum_decode_errors_total counter\n";
    os << "argentum_decode_errors_total " << metrics.decode_errors << "\n";
    os << "# TYPE argentum_order_requests_total counter\n";
    os << "argentum_order_requests_total " << metrics.order_requests << "\n";
    os << "# TYPE argentum_order_accepted_total counter\n";
    os << "argentum_order_accepted_total " << metrics.order_accepted << "\n";
    os << "# TYPE argentum_order_rejected_total counter\n";
    os << "argentum_order_rejected_total " << metrics.order_rejected << "\n";
    os << "# TYPE argentum_auth_failures_total counter\n";
    os << "argentum_auth_failures_total " << metrics.auth_failures << "\n";
    os << "# TYPE argentum_rate_limited_total counter\n";
    os << "argentum_rate_limited_total " << metrics.rate_limited << "\n";
    os << "# TYPE argentum_active_orders gauge\n";
    os << "argentum_active_orders " << active_orders << "\n";
    os << "# TYPE argentum_tracked_symbols gauge\n";
    os << "argentum_tracked_symbols " << metrics.tracked_symbols << "\n";
    return os.str();
}

} // namespace argentum::api
