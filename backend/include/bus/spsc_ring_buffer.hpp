#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace argentum::bus {

template<size_t Capacity, size_t MessageSize>
class SpscRingBuffer {
public:
    static_assert(Capacity > 1, "SpscRingBuffer capacity must be greater than one.");
    static_assert((Capacity & (Capacity - 1)) == 0, "SpscRingBuffer capacity must be a power of two.");

    struct alignas(64) Slot {
        uint8_t data[MessageSize]{};
        size_t size = 0;
        std::atomic<bool> ready{false};
    };

    explicit SpscRingBuffer(size_t logical_capacity = Capacity)
        : logical_capacity_(normalize_capacity(logical_capacity)),
          mask_(logical_capacity_ - 1) {}

    bool try_publish(const void* data, size_t size) {
        if (!data || size == 0 || size > MessageSize) {
            return false;
        }

        const uint64_t write = write_pos_.load(std::memory_order_relaxed);
        Slot& slot = slots_[write & mask_];
        if (slot.ready.load(std::memory_order_acquire)) {
            return false;
        }

        std::memcpy(slot.data, data, size);
        slot.size = size;
        slot.ready.store(true, std::memory_order_release);
        write_pos_.store(write + 1, std::memory_order_release);
        return true;
    }

    bool try_consume(void* out_data, size_t* out_size) {
        if (!out_size) {
            return false;
        }

        uint64_t read = read_pos_.load(std::memory_order_relaxed);
        for (;;) {
            Slot& slot = slots_[read & mask_];
            if (!slot.ready.load(std::memory_order_acquire)) {
                return false;
            }

            if (!read_pos_.compare_exchange_weak(
                    read,
                    read + 1,
                    std::memory_order_acq_rel,
                    std::memory_order_relaxed)) {
                continue;
            }

            const size_t size = slot.size;
            if (out_data) {
                if (*out_size < size) {
                    return false;
                }
                std::memcpy(out_data, slot.data, size);
            }
            *out_size = size;
            slot.ready.store(false, std::memory_order_release);
            return true;
        }
    }

    bool try_discard_oldest(size_t* out_size = nullptr) {
        uint64_t read = read_pos_.load(std::memory_order_relaxed);
        for (;;) {
            Slot& slot = slots_[read & mask_];
            if (!slot.ready.load(std::memory_order_acquire)) {
                return false;
            }

            if (!read_pos_.compare_exchange_weak(
                    read,
                    read + 1,
                    std::memory_order_acq_rel,
                    std::memory_order_relaxed)) {
                continue;
            }

            if (out_size) {
                *out_size = slot.size;
            }
            slot.ready.store(false, std::memory_order_release);
            return true;
        }
    }

    [[nodiscard]] size_t size_approx() const {
        const uint64_t write = write_pos_.load(std::memory_order_acquire);
        const uint64_t read = read_pos_.load(std::memory_order_acquire);
        const uint64_t delta = (write > read) ? (write - read) : 0;
        return static_cast<size_t>(delta > logical_capacity_ ? logical_capacity_ : delta);
    }

    [[nodiscard]] size_t capacity() const {
        return logical_capacity_;
    }

private:
    static size_t normalize_capacity(size_t requested) {
        size_t normalized = 1;
        while (normalized < requested && normalized < Capacity) {
            normalized <<= 1;
        }
        if (normalized > Capacity) {
            normalized = Capacity;
        }
        return normalized;
    }

    alignas(64) std::atomic<uint64_t> write_pos_{0};
    alignas(64) std::atomic<uint64_t> read_pos_{0};
    Slot slots_[Capacity]{};
    const size_t logical_capacity_;
    const size_t mask_;
};

} // namespace argentum::bus
