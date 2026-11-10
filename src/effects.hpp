#pragma once

#include "resonant/bytecode.hpp"

namespace resonant::dsp {
inline constexpr std::uint32_t limiter_latency = 64;
inline constexpr std::uint32_t halfband_length = 63;
inline constexpr std::uint32_t oversampling_latency = (halfband_length - 1) / 2;
inline constexpr std::uint32_t tank_words = 65536;
inline constexpr std::uint32_t hq_halfband_length = 129;
inline constexpr std::uint32_t hq_state_words = 6 * hq_halfband_length;
inline constexpr std::uint32_t hq_latency = 112;
using HqHalfband = std::array<float, hq_halfband_length>;

[[nodiscard]] const HqHalfband& hq_halfband();
[[nodiscard]] const HqHalfband& hq_guard();
[[nodiscard]] float oversaturate_hq(const HqHalfband& coeff, const HqHalfband& guard, std::span<float> state,
                                   std::array<std::uint32_t, 6>& cursors, float input, float drive) noexcept;

struct Tank {
    std::array<std::uint32_t, 6> start{};
    std::array<std::uint32_t, 6> length{};
    std::array<std::uint32_t, 6> cursor{};
    std::array<float, 4> damp{};

    void configure(std::uint32_t sample_rate) noexcept;
    [[nodiscard]] float process(std::span<float> buffer, float input, float feedback) noexcept;
};

using Halfband = std::array<float, halfband_length>;
[[nodiscard]] const Halfband& halfband();

[[nodiscard]] float saturation(float value, float drive) noexcept;
[[nodiscard]] float oversaturate(const Halfband& coeff, std::span<float> state, std::uint32_t& cursor,
                                float input, float drive) noexcept;
[[nodiscard]] float limit(std::span<float> state, std::uint32_t& cursor, float input, float threshold,
                          float release, double inverse_rate) noexcept;
}
