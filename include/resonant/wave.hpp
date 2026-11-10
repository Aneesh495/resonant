#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace resonant {
enum class Encoding : std::uint16_t { pcm16, pcm24, pcm32, float32, float64 };

struct WaveView {
    Encoding encoding;
    std::uint16_t channels;
    std::uint32_t rate;
    std::span<const std::byte> payload;
    [[nodiscard]] std::size_t frames() const;
    [[nodiscard]] double sample(std::size_t frame, std::size_t channel) const;
};

struct WaveOptions {
    Encoding encoding = Encoding::pcm24;
    std::uint32_t rate = 48000;
    std::uint16_t channels = 2;
    // Zero disables TPDF dither. Other values seed the reproducible generator.
    std::uint32_t dither_seed = 0;
    std::string_view comment{};
};

[[nodiscard]] WaveView parse_wave(std::span<const std::byte> bytes);
[[nodiscard]] std::vector<std::byte> encode_wave(std::span<const double> samples, WaveOptions options);
[[nodiscard]] std::vector<double> map_channels(const WaveView& wave, std::uint16_t channels);
}
