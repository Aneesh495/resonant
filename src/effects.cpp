#include "effects.hpp"

#include "dsp.hpp"
#include "numeric.hpp"

#include <numbers>

namespace resonant::dsp {
namespace {
float fir(const Halfband& coeff, std::span<const float> delay, std::uint32_t head) noexcept {
    float result = 0;
    for (std::size_t i = 0; i < coeff.size(); ++i) {
        result += coeff[i] * delay[head];
        head = head == 0 ? halfband_length - 1 : head - 1;
    }
    return result;
}
}

const Halfband& halfband() {
    static const auto coefficients = [] {
        constexpr auto middle = (halfband_length - 1) / 2;
        Halfband taps{};
        double total = 0;
        for (std::size_t i = 0; i < taps.size(); ++i) {
            const auto offset = static_cast<double>(i) - middle;
            const auto phase = 2 * std::numbers::pi * static_cast<double>(i) / (taps.size() - 1);
            const auto window = 0.42 - 0.5 * std::cos(phase) + 0.08 * std::cos(2 * phase);
            const auto ideal = i == middle ? 0.5 : std::sin(std::numbers::pi * offset / 2) / (std::numbers::pi * offset);
            taps[i] = static_cast<float>(window * ideal);
            total += window * ideal;
        }
        for (auto& tap : taps) tap /= static_cast<float>(total);
        return taps;
    }();
    return coefficients;
}

float saturation(float value, float drive) noexcept {
    const auto level = value * std::clamp(drive, 0.0F, 64.0F);
    return level / (1.0F + std::abs(level));
}

float oversaturate(const Halfband& coeff, std::span<float> state, std::uint32_t& cursor,
                  float input, float drive) noexcept {
    auto up = state.first(halfband_length);
    auto down = state.subspan(halfband_length, halfband_length);
    up[cursor] = input;
    down[cursor] = saturation(2 * fir(coeff, up, cursor), drive);
    const auto result = fir(coeff, down, cursor);
    if (++cursor == halfband_length) cursor = 0;
    up[cursor] = 0;
    down[cursor] = saturation(2 * fir(coeff, up, cursor), drive);
    if (++cursor == halfband_length) cursor = 0;
    return result;
}

float limit(std::span<float> state, std::uint32_t& cursor, float input, float threshold,
            float release, double inverse_rate) noexcept {
    const auto delayed = state[cursor];
    float maximum = std::abs(input);
    for (std::size_t i = 0; i < limiter_latency; ++i) maximum = std::max(maximum, std::abs(state[i]));
    state[cursor] = input;
    if (++cursor == limiter_latency) cursor = 0;
    const auto ceiling = std::clamp(threshold, 0.0001F, 1.0F);
    const auto target = maximum <= ceiling ? 1.0F : ceiling / maximum;
    auto& gain = state[limiter_latency];
    gain = target <= gain ? target : envelope(gain, target, release, inverse_rate);
    return std::clamp(delayed * gain, -ceiling, ceiling);
}

void Tank::configure(std::uint32_t sample_rate) noexcept {
    constexpr std::array milliseconds{29U, 37U, 43U, 53U, 5U, 7U};
    std::uint32_t offset = 0;
    for (std::size_t i = 0; i < length.size(); ++i) {
        start[i] = offset;
        length[i] = (sample_rate * milliseconds[i] / 1000U) | 1U;
        offset += length[i];
    }
}

float Tank::process(std::span<float> buffer, float input, float feedback) noexcept {
    const auto gain = std::clamp(feedback, 0.0F, 0.89F);
    float sum = 0;
    for (std::size_t i = 0; i < damp.size(); ++i) {
        auto& sample = buffer[start[i] + cursor[i]];
        const auto tap = sample;
        damp[i] = numeric::bounded(damp[i] + 0.30F * (tap - damp[i]));
        sample = numeric::bounded(input + gain * damp[i]);
        if (++cursor[i] == length[i]) cursor[i] = 0;
        sum += tap;
    }
    float signal = 0.25F * sum;
    for (std::size_t i = damp.size(); i < length.size(); ++i) {
        auto& sample = buffer[start[i] + cursor[i]];
        const auto out = sample - 0.5F * signal;
        sample = numeric::bounded(signal + 0.5F * out);
        if (++cursor[i] == length[i]) cursor[i] = 0;
        signal = out;
    }
    return signal;
}
}
