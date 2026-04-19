#include "resonant/spectral.hpp"
#include "test.hpp"

#include <array>
#include <numbers>
#include <random>

using namespace resonant;

int main() {
    std::mt19937 rng(9001);
    const auto sample = [&] { return static_cast<float>(rng() % 4096) / 2048 - 1; };
    for (const std::size_t n : {8U, 32U, 128U, 512U}) {
        const Fft plan(n);
        std::vector<std::complex<float>> x(n);
        for (auto& c : x) c = {sample(), sample()};
        const auto original = x;
        CHECK(plan.transform(x));
        for (std::size_t i = 0; i < n; ++i) {
            std::complex<double> direct{};
            for (std::size_t j = 0; j < n; ++j) {
                const auto phase = -2 * std::numbers::pi * static_cast<double>(i*j) / static_cast<double>(n);
                direct += std::complex<double>(original[j]) * std::polar(1.0, phase);
            }
            CHECK(std::abs(std::complex<double>(x[i]) - direct) < 2e-5);
        }
        CHECK(plan.transform(x, true));
        for (std::size_t i = 0; i < n; ++i) CHECK(std::abs(x[i] - original[i]) < 1e-6F);
        CHECK(!plan.transform(std::span(x).first(n-1)));
    }
    rejects([] { (void)Fft(12); });

    std::array<float, 179> impulse{};
    std::array<float, 1025> signal{};
    for (auto& x : signal) x = sample();
    for (auto& x : impulse) x = 0.02F * sample();
    for (const std::size_t p : {32U, 64U, 128U, 256U, 512U}) {
        Convolver convolution(p, impulse);
        CHECK(convolution.latency() == p);
        for (std::size_t i = 0; i < signal.size() + impulse.size() + p; ++i) {
            const auto value = convolution.process(i < signal.size() ? signal[i] : 0.0F);
            double direct = 0;
            if (i >= p) {
                for (std::size_t k = 0; k < impulse.size() && k <= i-p; ++k) {
                    const auto j = i-p-k;
                    if (j < signal.size()) direct += static_cast<double>(signal[j]) * impulse[k];
                }
            }
            const auto dry = i >= p && i - p < signal.size() ? signal[i - p] : 0;
            CHECK(value.dry == dry && std::abs(value.wet - direct) < 2e-6);
        }
    }
    for (const auto rate : {44100U, 48000U, 96000U}) {
        const auto early = room_impulse(false, rate);
        const auto hall = room_impulse(true, rate);
        CHECK(early.size() == 512 && hall.size() == 1024 && early.front() == 0.43F);
        (void)Convolver(64, early);
        (void)Convolver(128, hall);
    }
}
