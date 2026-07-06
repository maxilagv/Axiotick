#pragma once

#include "bus/message_bus.hpp"
#include "bus/message_protocol.hpp"
#include "core/types.h"
#include "datafeed/market_parser.h"

#include <memory>
#include <string>

namespace argentum::datafeed {

class FeedPlayer {
public:
    FeedPlayer(std::shared_ptr<bus::MessageBus> bus, std::string topic);

    size_t play_file(const std::string& path, FeedFormat format, uint32_t throttle_us);

private:
    std::shared_ptr<bus::MessageBus> bus_;
    std::string topic_;
};

} // namespace argentum::datafeed
