#pragma once

#include <complex>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace resonant {
class Fft {
public:
    explicit Fft(std::size_t size);
    [[nodiscard]] std::size_t size() const noexcept { return order_.size(); }
    [[nodiscard]] bool transform(std::span<std::complex<float>> data, bool inverse = false) const noexcept;

private:
    std::vector<std::size_t> order_;
    std::vector<std::complex<float>> roots_;
};

struct ConvolutionSample {
    float wet;
    float dry;
};

class Convolver {
public:
    Convolver(std::size_t partition, std::span<const float> impulse);
    [[nodiscard]] ConvolutionSample process(float input) noexcept;
    [[nodiscard]] std::size_t latency() const noexcept { return partition_; }
    [[nodiscard]] std::size_t partitions() const noexcept { return partitions_; }

private:
    void complete() noexcept;

    std::size_t partition_;
    std::size_t partitions_;
    Fft plan_;
    std::vector<std::complex<float>> response_;
    std::vector<std::complex<float>> history_;
    std::vector<std::complex<float>> sum_;
    std::vector<float> block_;
    std::vector<float> ready_;
    std::vector<float> overlap_;
    std::size_t position_ = 0;
    std::size_t head_ = 0;
};

[[nodiscard]] std::vector<float> room_impulse(bool hall, std::uint32_t sample_rate);
}
