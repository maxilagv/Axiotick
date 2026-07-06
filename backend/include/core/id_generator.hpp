#pragma once

#include <atomic>
#include <cstdint>

namespace argentum::core {

class OrderIdGenerator {
public:
    explicit OrderIdGenerator(uint64_t start = 1) : counter_(start) {}

    uint64_t next() {
        return counter_.fetch_add(1, std::memory_order_relaxed);
    }

    static OrderIdGenerator& global() {
        static OrderIdGenerator generator;
        return generator;
    }

private:
    std::atomic<uint64_t> counter_;
};

inline uint64_t next_order_id() {
    return OrderIdGenerator::global().next();
}

} // namespace argentum::core
