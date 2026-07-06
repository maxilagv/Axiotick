#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace argentum::engine {

template<typename T, size_t PoolSize>
class PoolAllocator {
public:
    static_assert(PoolSize > 0, "PoolAllocator requires a positive capacity.");
    static_assert(PoolSize <= static_cast<size_t>(INT32_MAX), "PoolAllocator capacity exceeds index range.");

    PoolAllocator() {
        reset();
    }

    PoolAllocator(const PoolAllocator&) = delete;
    PoolAllocator& operator=(const PoolAllocator&) = delete;

    template<typename... Args>
    int32_t try_emplace(Args&&... args) {
        if (free_count_ == 0) {
            return -1;
        }

        const int32_t index = free_list_[--free_count_];
        pool_[static_cast<size_t>(index)] = T{std::forward<Args>(args)...};
        used_[static_cast<size_t>(index)] = true;
        return index;
    }

    void deallocate(int32_t index) {
        if (index < 0 || static_cast<size_t>(index) >= PoolSize) {
            return;
        }
        if (!used_[static_cast<size_t>(index)]) {
            return;
        }

        pool_[static_cast<size_t>(index)] = T{};
        used_[static_cast<size_t>(index)] = false;
        free_list_[free_count_++] = index;
    }

    [[nodiscard]] bool in_use(int32_t index) const {
        return index >= 0 &&
               static_cast<size_t>(index) < PoolSize &&
               used_[static_cast<size_t>(index)];
    }

    [[nodiscard]] T& at(int32_t index) {
        return pool_[static_cast<size_t>(index)];
    }

    [[nodiscard]] const T& at(int32_t index) const {
        return pool_[static_cast<size_t>(index)];
    }

    [[nodiscard]] size_t capacity() const {
        return PoolSize;
    }

    [[nodiscard]] size_t available() const {
        return free_count_;
    }

private:
    void reset() {
        free_count_ = PoolSize;
        for (size_t i = 0; i < PoolSize; ++i) {
            free_list_[i] = static_cast<int32_t>(PoolSize - 1 - i);
            used_[i] = false;
        }
    }

    std::array<T, PoolSize> pool_{};
    std::array<bool, PoolSize> used_{};
    std::array<int32_t, PoolSize> free_list_{};
    size_t free_count_ = 0;
};

} // namespace argentum::engine
