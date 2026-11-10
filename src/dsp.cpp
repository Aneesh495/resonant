#include "dsp.hpp"

#include "numeric.hpp"

#include <bit>
#include <numbers>

namespace resonant::dsp {
namespace {
constexpr double tau = 2 * std::numbers::pi;
constexpr double phase_scale = 0x1p32;

float lookup(const std::array<float, table_size>& table, std::uint32_t phase) noexcept {
    constexpr auto fractional_bits = 32 - table_bits;
    constexpr auto mask = (std::uint32_t{1} << fractional_bits) - 1;
    const auto i = phase >> fractional_bits;
    const auto fraction = static_cast<float>(phase & mask) / static_cast<float>(mask + 1);
    const auto next = (i + 1) & (table_size - 1);
    return table[i] + fraction * (table[next] - table[i]);
}

float bandlimited(const std::array<std::array<float, table_size>, bands>& table,
                  std::uint32_t phase, std::uint32_t step) noexcept {
    if (step == 0) return table.back()[phase >> (32 - table_bits)];
    const auto band = 31U - static_cast<unsigned>(std::bit_width(step));
    const auto index = std::min<std::size_t>(band, bands - 1);
    const auto value = lookup(table[index], phase);
    if (index == 0 || band >= bands) return value;
    const auto largest_step = std::uint32_t{1} << (31U - band);
    const auto weight = 2.0F - 2.0F * static_cast<float>(step) / static_cast<float>(largest_step);
    return lookup(table[index - 1], phase) * (1.0F - weight) + value * weight;
}
}

Tables::Tables() {
    for (std::size_t i = 0; i < table_size; ++i) {
        const auto phase = tau * static_cast<double>(i) / table_size;
        sine[i] = static_cast<float>(std::sin(phase));
        double s = 0;
        double t = 0;
        unsigned harmonic = 1;
        for (std::size_t b = 0; b < bands; ++b) {
            const auto limit = 1U << b;
            for (; harmonic <= limit; ++harmonic) {
                const auto component = std::sin(phase * harmonic);
                s -= component / harmonic;
                if ((harmonic & 1U) != 0) {
                    const auto sign = harmonic % 4 == 1 ? 1.0 : -1.0;
                    t += sign * component / (harmonic * harmonic);
                }
            }
            saw[b][i] = static_cast<float>(s * (2 / std::numbers::pi));
            triangle[b][i] = static_cast<float>(t * (8 / (std::numbers::pi * std::numbers::pi)));
        }
    }
}

const Tables& tables() {
    static const Tables collection;
    return collection;
}

std::uint32_t phase_step(double hertz, double inverse_rate) noexcept {
    return static_cast<std::uint32_t>(std::clamp(hertz * inverse_rate, 0.0, 0.45) * phase_scale + 0.5);
}

std::uint32_t phase_offset(float cycles) noexcept {
    const auto fraction = static_cast<double>(cycles) - std::floor(static_cast<double>(cycles));
    return static_cast<std::uint32_t>(fraction * phase_scale);
}

float wave(const Tables& t, Op op, std::uint32_t phase, std::uint32_t step, float shape) noexcept {
    switch (op) {
    case Op::saw:
    case Op::poly_saw:
        return bandlimited(t.saw, phase, step);
    case Op::triangle:
    case Op::poly_triangle:
        return bandlimited(t.triangle, phase, step);
    case Op::pulse:
    case Op::poly_pulse: {
        const auto duty = std::clamp(shape, 0.02F, 0.98F);
        return bandlimited(t.saw, phase - phase_offset(duty), step) -
               bandlimited(t.saw, phase, step) + 2.0F * duty - 1.0F;
    }
    default:
        return lookup(t.sine, phase + phase_offset(shape));
    }
}

float filter(Op op, float* state, float signal, float hertz, float resonance, double inverse_rate) noexcept {
    const auto normalized = std::clamp(static_cast<double>(hertz) * inverse_rate, 0.0001, 0.45);
    const auto g = static_cast<float>(std::tan(std::numbers::pi * normalized));
    const auto k = 1.0F / std::clamp(resonance, 0.25F, 12.0F);
    const auto a = 1.0F / (1.0F + g * (g + k));
    const auto band = a * (state[0] + g * (signal - state[1]));
    const auto low = state[1] + g * band;
    state[0] = numeric::bounded(2 * band - state[0]);
    state[1] = numeric::bounded(2 * low - state[1]);
    switch (op) {
    case Op::lowpass: return low;
    case Op::bandpass: return band;
    default: return signal - k * band - low;
    }
}

float envelope(float previous, float target, float seconds, double inverse_rate) noexcept {
    const double time = std::clamp(static_cast<double>(seconds), 0.00025, 10.0);
    const auto coefficient = static_cast<float>(-std::expm1(-inverse_rate / time));
    return previous + coefficient * (target - previous);
}

float fractional_delay(std::span<const float> ring, std::uint32_t cursor, float frames) noexcept {
    const auto distance = std::clamp(frames, 1.0F, static_cast<float>(ring.size() - 1));
    const auto integral = static_cast<std::size_t>(distance);
    const auto fraction = distance - static_cast<float>(integral);
    const auto a = cursor >= integral ? cursor - integral : cursor + ring.size() - integral;
    const auto b = a == 0 ? ring.size() - 1 : a - 1;
    return ring[a] + fraction * (ring[b] - ring[a]);
}
}
