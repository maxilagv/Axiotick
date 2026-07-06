#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>

namespace argentum::persist {

template<typename T, size_t N>
class LockFreeRingBuffer {
public:
    static_assert(N > 1, "LockFreeRingBuffer capacity must be greater than 1.");
    static_assert((N & (N - 1)) == 0, "LockFreeRingBuffer capacity must be a power of two.");

    LockFreeRingBuffer() {
        for (size_t i = 0; i < N; ++i) {
            cells_[i].sequence.store(i, std::memory_order_relaxed);
        }
    }

    LockFreeRingBuffer(const LockFreeRingBuffer&) = delete;
    LockFreeRingBuffer& operator=(const LockFreeRingBuffer&) = delete;

    bool try_push(const T& value) {
        return try_push_impl(value);
    }

    bool try_push(T&& value) {
        return try_push_impl(std::move(value));
    }

    bool try_pop(T* out) {
        if (!out) {
            return false;
        }

        Cell* cell = nullptr;
        uint64_t pos = read_pos_.load(std::memory_order_relaxed);
        for (;;) {
            cell = &cells_[pos & mask_];
            const uint64_t sequence = cell->sequence.load(std::memory_order_acquire);
            const intptr_t diff = static_cast<intptr_t>(sequence) -
                                  static_cast<intptr_t>(pos + 1);

            if (diff == 0) {
                if (read_pos_.compare_exchange_weak(
                        pos,
                        pos + 1,
                        std::memory_order_relaxed,
                        std::memory_order_relaxed)) {
                    break;
                }
                continue;
            }

            if (diff < 0) {
                return false;
            }

            pos = read_pos_.load(std::memory_order_relaxed);
        }

        *out = std::move(cell->value);
        cell->sequence.store(pos + N, std::memory_order_release);
        return true;
    }

    [[nodiscard]] static constexpr size_t capacity() {
        return N;
    }

    [[nodiscard]] size_t size_approx() const {
        const uint64_t write = write_pos_.load(std::memory_order_acquire);
        const uint64_t read = read_pos_.load(std::memory_order_acquire);
        if (write <= read) {
            return 0;
        }
        const uint64_t delta = write - read;
        return static_cast<size_t>(delta > N ? N : delta);
    }

private:
    struct Cell {
        std::atomic<uint64_t> sequence{0};
        T value{};
    };

    template<typename U>
    bool try_push_impl(U&& value) {
        Cell* cell = nullptr;
        uint64_t pos = write_pos_.load(std::memory_order_relaxed);
        for (;;) {
            cell = &cells_[pos & mask_];
            const uint64_t sequence = cell->sequence.load(std::memory_order_acquire);
            const intptr_t diff = static_cast<intptr_t>(sequence) -
                                  static_cast<intptr_t>(pos);

            if (diff == 0) {
                if (write_pos_.compare_exchange_weak(
                        pos,
                        pos + 1,
                        std::memory_order_relaxed,
                        std::memory_order_relaxed)) {
                    break;
                }
                continue;
            }

            if (diff < 0) {
                return false;
            }

            pos = write_pos_.load(std::memory_order_relaxed);
        }

        cell->value = std::forward<U>(value);
        cell->sequence.store(pos + 1, std::memory_order_release);
        return true;
    }

    static constexpr size_t mask_ = N - 1;

    alignas(64) std::atomic<uint64_t> write_pos_{0};
    alignas(64) std::atomic<uint64_t> read_pos_{0};
    std::array<Cell, N> cells_{};
};

} // namespace argentum::persist
