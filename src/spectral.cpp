#include "resonant/spectral.hpp"

#include "numeric.hpp"

#include <bit>
#include <numbers>
#include <stdexcept>

namespace resonant {
namespace {
std::size_t checked_fft(std::size_t size) {
    if (size < 2 || size > 8192 || !std::has_single_bit(size)) throw std::runtime_error("invalid radix-2 FFT size");
    return size;
}

std::size_t checked_partition(std::size_t size) {
    if (size < 32 || size > 512 || !std::has_single_bit(size)) throw std::runtime_error("invalid convolution partition");
    return size;
}

std::size_t partition_count(std::size_t partition, std::span<const float> impulse) {
    if (impulse.empty() || impulse.size() > partition * 64) throw std::runtime_error("invalid impulse extent");
    double mass = 0;
    for (const auto v : impulse) {
        if (!std::isfinite(v)) throw std::runtime_error("nonfinite impulse coefficient");
        mass += std::abs(v);
    }
    if (mass > 8) throw std::runtime_error("impulse exceeds absolute gain budget");
    return (impulse.size() + partition - 1) / partition;
}
}

Fft::Fft(std::size_t size) : order_(checked_fft(size)), roots_(size / 2) {
    const auto bits = std::countr_zero(size);
    for (std::size_t i = 0; i < size; ++i) {
        std::size_t value = 0;
        for (int bit = 0; bit < bits; ++bit) value = (value << 1) | ((i >> bit) & 1U);
        order_[i] = value;
    }
    for (std::size_t i = 0; i < roots_.size(); ++i) {
        const auto phase = -2 * std::numbers::pi * static_cast<double>(i) / static_cast<double>(size);
        roots_[i] = {static_cast<float>(std::cos(phase)), static_cast<float>(std::sin(phase))};
    }
}

bool Fft::transform(std::span<std::complex<float>> data, bool inverse) const noexcept {
    const auto n = order_.size();
    if (data.size() != n) return false;
    for (std::size_t i = 0; i < n; ++i) {
        if (i < order_[i]) std::swap(data[i], data[order_[i]]);
    }
    for (std::size_t width = 2; width <= n; width *= 2) {
        const auto half = width / 2;
        const auto step = n / width;
        for (std::size_t start = 0; start < n; start += width) {
            for (std::size_t k = 0; k < half; ++k) {
                const auto twiddle = inverse ? std::conj(roots_[k * step]) : roots_[k * step];
                const auto a = data[start + k];
                const auto b = data[start + k + half] * twiddle;
                data[start + k] = a + b;
                data[start + k + half] = a - b;
            }
        }
    }
    if (inverse) {
        const auto scale = 1.0F / static_cast<float>(n);
        for (auto& sample : data) sample *= scale;
    }
    return true;
}

Convolver::Convolver(std::size_t partition, std::span<const float> impulse)
    : partition_(checked_partition(partition)), partitions_(partition_count(partition_, impulse)),
      plan_(2 * partition_), response_(partitions_ * plan_.size()), history_(response_.size()),
      sum_(plan_.size()), block_(partition_), ready_(partition_), overlap_(partition_) {
    for (std::size_t i = 0; i < partitions_; ++i) {
        auto spectrum = std::span(response_).subspan(i * plan_.size(), plan_.size());
        const auto extent = std::min(impulse.size() - i * partition_, partition_);
        for (std::size_t j = 0; j < extent; ++j) spectrum[j] = impulse[i * partition_ + j];
        (void)plan_.transform(spectrum);
    }
}

ConvolutionSample Convolver::process(float input) noexcept {
    const ConvolutionSample sample{ready_[position_], block_[position_]};
    block_[position_] = numeric::bounded(input);
    if (++position_ == partition_) {
        complete();
        position_ = 0;
    }
    return sample;
}

void Convolver::complete() noexcept {
    const auto n = plan_.size();
    auto input = std::span(history_).subspan(head_ * n, n);
    for (std::size_t i = 0; i < partition_; ++i) {
        input[i] = block_[i];
        input[i + partition_] = {};
    }
    (void)plan_.transform(input);
    std::ranges::fill(sum_, std::complex<float>{});
    auto previous = head_;
    for (std::size_t i = 0; i < partitions_; ++i) {
        for (std::size_t bin = 0; bin < n; ++bin) {
            sum_[bin] += response_[i * n + bin] * history_[previous * n + bin];
        }
        previous = previous == 0 ? partitions_ - 1 : previous - 1;
    }
    (void)plan_.transform(sum_, true);
    for (std::size_t i = 0; i < partition_; ++i) {
        ready_[i] = numeric::bounded(sum_[i].real() + overlap_[i]);
        overlap_[i] = numeric::bounded(sum_[i + partition_].real());
    }
    if (++head_ == partitions_) head_ = 0;
}

std::vector<float> room_impulse(bool hall, std::uint32_t sample_rate) {
    if (sample_rate < 8000 || sample_rate > 192000) throw std::runtime_error("invalid impulse sample rate");
    const std::size_t size = hall ? 1024 : 512;
    std::vector<float> impulse(size);
    const double scale = static_cast<double>(sample_rate) / 48000.0;
    constexpr std::array<std::size_t, 6> taps{0, 71, 137, 233, 353, 479};
    constexpr std::array amplitudes{0.43F, 0.26F, -0.20F, 0.17F, -0.12F, 0.09F};
    for (std::size_t i = 0; i < taps.size(); ++i) {
        const auto time = static_cast<std::size_t>(static_cast<double>(taps[i]) * scale * (hall ? 1.5 : 0.82) + 0.5);
        if (time < size) impulse[time] += amplitudes[i];
    }
    std::uint32_t random = hall ? 0x8df12e5bU : 0x5531c711U;
    float diffuse = 0;
    for (std::size_t i = 1; i < size; ++i) {
        random = random * 1664525U + 1013904223U;
        const float draw = static_cast<float>(random >> 8) * 0x1p-23F - 1.0F;
        diffuse = 0.65F * diffuse + 0.35F * draw;
        const auto position = static_cast<float>(i) / static_cast<float>(size);
        impulse[i] += diffuse * (hall ? 0.036F : 0.025F) * (1 - position) * (1 - position);
    }
    return impulse;
}
}
