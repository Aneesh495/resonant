#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <type_traits>

namespace resonant {
template<class T, std::uint32_t Capacity>
class Spsc {
    static_assert(Capacity != 0 && (Capacity & (Capacity - 1)) == 0 && Capacity < (1U << 31));
    static_assert(std::is_trivially_copyable_v<T>);
    static_assert(std::atomic<std::uint32_t>::is_always_lock_free);

public:
    bool push(const T& value) noexcept {
        const auto write = write_.load(std::memory_order_relaxed);
        if (write - read_.load(std::memory_order_acquire) == Capacity) return false;
        items_[write & (Capacity - 1)] = value;
        write_.store(write + 1, std::memory_order_release);
        return true;
    }

    bool peek(T& value) const noexcept {
        const auto read = read_.load(std::memory_order_relaxed);
        if (read == write_.load(std::memory_order_acquire)) return false;
        value = items_[read & (Capacity - 1)];
        return true;
    }

    void consume() noexcept {
        const auto read = read_.load(std::memory_order_relaxed);
        read_.store(read + 1, std::memory_order_release);
    }

    bool pop(T& value) noexcept {
        if (!peek(value)) return false;
        consume();
        return true;
    }

private:
    alignas(64) std::atomic<std::uint32_t> read_{0};
    alignas(64) std::atomic<std::uint32_t> write_{0};
    std::array<T, Capacity> items_{};
};
}
