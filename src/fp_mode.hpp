#pragma once

#include <cstdint>

#if defined(__x86_64__) && defined(__SSE2__)
#include <xmmintrin.h>
#endif

namespace resonant::numeric {
#if defined(__aarch64__)
inline constexpr bool native_float_mode = true;
#elif defined(__x86_64__) && defined(__SSE2__)
inline constexpr bool native_float_mode = true;
#else
inline constexpr bool native_float_mode = false;
#endif

class FloatMode {
public:
#if defined(__aarch64__)
    FloatMode() noexcept {
        asm volatile("mrs %0, fpcr" : "=r"(original_) :: "memory");
        const auto desired = (original_ & ~(3ULL << 22U)) | (1ULL << 24U);
        changed_ = original_ != desired;
        if (changed_) write(desired);
    }
    ~FloatMode() {
        if (changed_) write(original_);
    }

private:
    static void write(std::uint64_t bits) noexcept {
        asm volatile("msr fpcr, %0" :: "r"(bits) : "memory");
    }
    std::uint64_t original_{};
    bool changed_{};
#elif defined(__x86_64__) && defined(__SSE2__)
    FloatMode() noexcept : original_(_mm_getcsr()) {
        const auto desired = (original_ & ~static_cast<unsigned>(_MM_ROUND_MASK)) | _MM_FLUSH_ZERO_ON;
        changed_ = original_ != desired;
        if (changed_) _mm_setcsr(desired);
    }
    ~FloatMode() {
        if (changed_) _mm_setcsr(original_);
    }

private:
    unsigned original_{};
    bool changed_{};
#else
    FloatMode() noexcept = default;
#endif

public:
    FloatMode(const FloatMode&) = delete;
    FloatMode& operator=(const FloatMode&) = delete;
};
}
