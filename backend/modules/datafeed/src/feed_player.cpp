#include "datafeed/feed_player.hpp"
#include "codec/market_tick_codec.hpp"

#include <chrono>
#include <fstream>
#include <thread>

namespace argentum::datafeed {

FeedPlayer::FeedPlayer(std::shared_ptr<bus::MessageBus> bus, std::string topic)
    : bus_(std::move(bus)), topic_(std::move(topic)) {}

static size_t trim_line(char* line, size_t len) {
    while (len > 0) {
        char c = line[len - 1];
        if (c == '\n' || c == '\r') {
            line[len - 1] = '\0';
            --len;
            continue;
        }
        break;
    }
    return len;
}

size_t FeedPlayer::play_file(const std::string& path, FeedFormat format, uint32_t throttle_us) {
    if (!bus_) return 0;

    std::ifstream file(path);
    if (!file.is_open()) return 0;

    size_t published = 0;
    std::string line;
    line.reserve(512);

    while (std::getline(file, line)) {
        if (line.empty()) continue;
        size_t len = line.size();
        line.push_back('\0');
        len = trim_line(line.data(), len);
        if (len == 0) {
            line.pop_back();
            continue;
        }

        MarketTick tick{};
        if (parse_market_message(format, line.data(), len, &tick) == ARGENTUM_OK) {
            std::vector<uint8_t> payload;
            ArgentumStatus status = ARGENTUM_ERR_INVALID;
#ifdef ARGENTUM_USE_FLATBUFFERS
            status = codec::encode_market_tick_flatbuffers(tick, &payload, false);
#else
            status = codec::encode_market_tick_legacy(tick, &payload);
#endif
            if (status == ARGENTUM_OK) {
                if (bus_->publish(topic_, payload.data(), payload.size()) == ARGENTUM_OK) {
                    ++published;
                }
            }
        }

        if (throttle_us > 0) {
            std::this_thread::sleep_for(std::chrono::microseconds(throttle_us));
        }

        line.pop_back();
    }

    return published;
}

} // namespace argentum::datafeed
