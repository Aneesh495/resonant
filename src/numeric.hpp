#pragma once

#include "resonant/bytecode.hpp"

#include <algorithm>
#include <cmath>

namespace resonant::numeric {
inline float bounded(float value) noexcept {
    if (!std::isfinite(value) || std::abs(value) < 0x1p-100F) return 0.0F;
    return std::clamp(value, -signal_limit, signal_limit);
}

inline float arithmetic(Op op, float x, float y) noexcept {
    switch (op) {
    case Op::add: return bounded(x + y);
    case Op::sub: return bounded(x - y);
    case Op::mul: return bounded(x * y);
    case Op::clip: return std::clamp(x, -std::abs(y), std::abs(y));
    default: return 0;
    }
}
}
