#include "dsp.hpp"

#include "numeric.hpp"

#if defined(__aarch64__) && defined(__ARM_NEON)
#include <arm_neon.h>
#elif defined(__SSE2__)
#include <emmintrin.h>
#endif

namespace resonant {
bool simd_available() noexcept {
#if (defined(__aarch64__) && defined(__ARM_NEON)) || defined(__SSE2__)
    return true;
#else
    return false;
#endif
}

namespace dsp {
namespace {
template<Op op>
void vector_kernel(float* dst, const float* x, const float* y, std::size_t size) noexcept {
    std::size_t i = 0;
#if defined(__aarch64__) && defined(__ARM_NEON)
    const auto limit = vdupq_n_f32(signal_limit);
    const auto lower = vdupq_n_f32(-signal_limit);
    const auto floor = vdupq_n_f32(0x1p-100F);
    for (; i + 4 <= size; i += 4) {
        const auto a = vld1q_f32(x + i);
        const auto b = vld1q_f32(y + i);
        float32x4_t value;
        if constexpr (op == Op::add) value = vaddq_f32(a, b);
        if constexpr (op == Op::sub) value = vsubq_f32(a, b);
        if constexpr (op == Op::mul) value = vmulq_f32(a, b);
        value = vbslq_f32(vcgeq_f32(vabsq_f32(value), floor), value, vdupq_n_f32(0));
        vst1q_f32(dst + i, vmaxq_f32(lower, vminq_f32(limit, value)));
    }
#elif defined(__SSE2__)
    const auto limit = _mm_set1_ps(signal_limit);
    const auto lower = _mm_set1_ps(-signal_limit);
    const auto sign = _mm_set1_ps(-0.0F);
    const auto floor = _mm_set1_ps(0x1p-100F);
    for (; i + 4 <= size; i += 4) {
        const auto a = _mm_loadu_ps(x + i);
        const auto b = _mm_loadu_ps(y + i);
        __m128 value;
        if constexpr (op == Op::add) value = _mm_add_ps(a, b);
        if constexpr (op == Op::sub) value = _mm_sub_ps(a, b);
        if constexpr (op == Op::mul) value = _mm_mul_ps(a, b);
        value = _mm_and_ps(value, _mm_cmpge_ps(_mm_andnot_ps(sign, value), floor));
        _mm_storeu_ps(dst + i, _mm_max_ps(lower, _mm_min_ps(limit, value)));
    }
#endif
    for (; i < size; ++i) dst[i] = numeric::arithmetic(op, x[i], y[i]);
}
}

bool vector_arithmetic(Op op, float* dst, const float* x, const float* y, std::size_t size) noexcept {
    if (!simd_available()) return false;
    switch (op) {
    case Op::add: vector_kernel<Op::add>(dst, x, y, size); return true;
    case Op::sub: vector_kernel<Op::sub>(dst, x, y, size); return true;
    case Op::mul: vector_kernel<Op::mul>(dst, x, y, size); return true;
    default: return false;
    }
}
}
}
