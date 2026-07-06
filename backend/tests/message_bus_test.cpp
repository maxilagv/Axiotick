#include "bus/message_bus.hpp"

#include <cassert>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

int main() {
    {
        argentum::bus::InprocBusConfig config;
        config.queue_capacity = 2;
        config.policy = argentum::bus::BackpressurePolicy::DropNewest;
        config.consumer_threads = 0;

        auto bus = argentum::bus::create_inproc_bus(config);
        const char payload[4] = {'t', 'e', 's', 't'};
        assert(bus->publish("market.ticks", payload, sizeof(payload)) == ARGENTUM_OK);
        assert(bus->publish("market.ticks", payload, sizeof(payload)) == ARGENTUM_OK);
        assert(bus->publish("market.ticks", payload, sizeof(payload)) == ARGENTUM_ERR_TIMEOUT);

        argentum::bus::TopicMetrics metrics{};
        assert(bus->get_metrics("market.ticks", &metrics));
        assert(metrics.queue_depth == 2);
        assert(metrics.drops == 1);
    }

    {
        argentum::bus::InprocBusConfig config;
        config.queue_capacity = 1024;
        config.policy = argentum::bus::BackpressurePolicy::DropNewest;
        config.consumer_threads = 1;
        config.busy_poll_during_market_hours = true;

        auto bus = argentum::bus::create_inproc_bus(config);
        std::vector<std::string> received;
        received.reserve(64);
        std::mutex received_mutex;

        bus->subscribe("market.ticks", [&](const void* data, size_t size) {
            std::lock_guard<std::mutex> lock(received_mutex);
            received.emplace_back(static_cast<const char*>(data), size);
        });

        for (int i = 0; i < 64; ++i) {
            const std::string payload = "msg-" + std::to_string(i);
            assert(bus->publish("market.ticks", payload.data(), payload.size()) == ARGENTUM_OK);
        }

        for (int wait = 0; wait < 100; ++wait) {
            {
                std::lock_guard<std::mutex> lock(received_mutex);
                if (received.size() >= 64) {
                    break;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        std::lock_guard<std::mutex> lock(received_mutex);
        assert(received.size() == 64);
        for (int i = 0; i < 64; ++i) {
            assert(received[static_cast<size_t>(i)] == ("msg-" + std::to_string(i)));
        }
    }

    return 0;
}
