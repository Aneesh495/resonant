#pragma once

#include "resonant/bytecode.hpp"

namespace resonant::dsp {
inline constexpr std::size_t table_bits = 12;
inline constexpr std::size_t table_size = 1U << table_bits;
inline constexpr std::size_t bands = 8;
using BandTable = std::array<std::array<float, table_size>, bands>;
inline constexpr std::uint32_t wavetable_words = bands * table_size + max_asset_samples;

class Wavetable {
public:
    explicit Wavetable(std::span<const float> samples);
    [[nodiscard]] float sample(std::uint32_t phase, std::uint32_t step) const noexcept;

private:
    BandTable bands_{};
};

struct Tables {
    std::array<float, table_size> sine{};
    BandTable saw{};
    BandTable triangle{};
    Tables();
};

[[nodiscard]] const Tables& tables();
[[nodiscard]] std::uint32_t phase_step(double hertz, double inverse_rate) noexcept;
[[nodiscard]] std::uint32_t phase_offset(float cycles) noexcept;
[[nodiscard]] float wave(const Tables& t, Op op, std::uint32_t phase, std::uint32_t step,
                         float shape) noexcept;
[[nodiscard]] float filter(Op op, float* integrators, float signal, float hertz,
                           float resonance, double inverse_rate) noexcept;
[[nodiscard]] float envelope(float previous, float target, float seconds, double inverse_rate) noexcept;
[[nodiscard]] float fractional_delay(std::span<const float> ring, std::uint32_t cursor,
                                     float frames) noexcept;
[[nodiscard]] bool vector_arithmetic(Op op, float* dst, const float* x, const float* y,
                                     std::size_t size) noexcept;
}
