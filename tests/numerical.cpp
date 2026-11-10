#include "resonant/engine.hpp"
#include "resonant/graph.hpp"
#include "test.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <numbers>
#include <string>

using namespace resonant;

namespace {
constexpr double pi = std::numbers::pi;
constexpr std::size_t fft_window = 4096;

void check_filters(std::uint32_t rate, unsigned bin, double cutoff, double q) {
    constexpr double amplitude = 0.05;
    const auto hz = static_cast<double>(rate) * bin / fft_window;
    const auto signal = "osc=sine(" + std::to_string(hz) + "); x=mul(osc,.05); ";
    const auto warp = std::tan(pi * hz / rate) / std::tan(pi * cutoff / rate);
    const std::complex<double> denominator{1 - warp * warp, warp / q};
    const std::array numerator{std::complex<double>{1, 0}, std::complex<double>{-warp * warp, 0},
                               std::complex<double>{0, warp}};
    const std::array names{"lowpass", "highpass", "bandpass"};
    for (std::size_t type = 0; type < names.size(); ++type) {
        const auto patch = compile(signal + "y=" + names[type] + "(x," + std::to_string(cutoff) + "," +
                                   std::to_string(q) + "); out(y);");
        Vm vm(patch.program, rate);
        std::array<float, 1024> block{};
        std::complex<double> observed{};
        for (std::size_t frame = 0; frame < fft_window * 2; frame += 512) {
            const auto stats = vm.render(block);
            CHECK(stats.clipped == 0);
            if (frame < fft_window) continue; // Drop the filter startup transient.
            for (std::size_t i = 0; i < 512; ++i) {
                const auto phase = -2 * pi * bin * static_cast<double>(frame + i) / fft_window;
                observed += static_cast<double>(block[i*2]) * std::polar(1.0, phase);
            }
        }
        const auto response = observed / std::complex<double>{0, -amplitude * fft_window / 2};
        CHECK(std::abs(response - numerator[type] / denominator) < .0008);
    }
}

void check_envelopes(std::uint32_t rate) {
    const auto patch = compile("param p=.2; x=audio(p); y=follower(x,.01,.05); z=compressor(x,.4,.01); out(y,z);");
    Vm vm(patch.program, rate);
    std::array<float, 1024> data{};
    for (unsigned n = 0; n < 80; ++n) (void)vm.render(data);
    CHECK(vm.event({0, EventType::parameter, 0, 0, name_key("p"), .8F}));
    (void)vm.render(data);
    for (std::size_t i = 0; i < 512; ++i) {
        const auto t = static_cast<double>(i+1) / rate;
        const auto follower = .8 - .6 * std::exp(-t / .01);
        const auto compressor = .8 * (.625 + .375 * std::exp(-t / .003));
        CHECK(std::abs(data[2*i] - follower) < 2e-5);
        CHECK(std::abs(data[2*i+1] - compressor) < 5e-5);
    }
    CHECK(vm.event({0, EventType::parameter, 0, 0, name_key("p"), .2F}));
    const auto elapsed = 512.0 / rate;
    const auto follower_at_switch = .8 - .6 * std::exp(-elapsed / .01);
    const auto gain_at_switch = .625 + .375 * std::exp(-elapsed / .003);
    (void)vm.render(data);
    for (std::size_t i = 0; i < 512; ++i) {
        const auto t = static_cast<double>(i+1) / rate;
        const auto follower = .2 + (follower_at_switch - .2) * std::exp(-t / .05);
        const auto compressor = .2 * (1 - (1 - gain_at_switch) * std::exp(-t / .01));
        CHECK(std::abs(data[2*i] - follower) < 2e-5);
        CHECK(std::abs(data[2*i+1] - compressor) < 5e-5);
    }

    const auto gate = compile("param p=.8; x=audio(p); g=gate(x,.4,.01); out(g);");
    Vm gated(gate.program, rate);
    (void)gated.render(data);
    for (std::size_t i = 0; i < 512; ++i) {
        const auto t = static_cast<double>(i+1) / rate;
        CHECK(std::abs(data[2*i] - .8 * (1 - std::exp(-t / .01))) < 2e-5);
    }
    CHECK(gated.event({0, EventType::parameter, 0, 0, name_key("p"), .2F}));
    (void)gated.render(data);
    for (std::size_t i = 0; i < 512; ++i) {
        const auto t = static_cast<double>(i+1) / rate;
        CHECK(std::abs(data[2*i] - .2 * (1 - std::exp(-elapsed / .01)) * std::exp(-t / .003)) < 2e-5);
    }
}

double largest_jump(std::uint32_t ramp) {
    const auto patch = compile("param p=0; y=audio(p); out(y);");
    Engine engine(patch.program, 48000);
    CHECK(engine.enqueue({13, EventType::parameter, 0, 0, name_key("p"), .5F, ramp}) == Enqueue::accepted);
    std::array<float, 256> samples{};
    engine.render(std::span(samples).first(48));
    engine.render(std::span(samples).subspan(48));
    float peak = 0;
    for (std::size_t i = 2; i < samples.size(); i += 2) peak = std::max(peak, std::abs(samples[i] - samples[i-2]));
    CHECK(samples[24] == 0 && samples[26] > 0 && samples.back() == .5F);
    return peak;
}

void check_cutoff_sweep() {
    const auto patch = compile("param cut=250; osc=sine(920); x=mul(osc,.02); "
                               "y=lowpass(x,cut,1.5); out(y);");
    Engine engine(patch.program, 48000);
    std::array<float, 256> output{};
    for (unsigned i = 0; i < 200; ++i) {
        if (i % 8 == 0) {
            const auto value = (i / 8) % 2 == 0 ? 14000.F : 250.F;
            CHECK(engine.enqueue({engine.clock() + 17, EventType::parameter, 0, 0, name_key("cut"),
                                  value, 1000}) == Enqueue::accepted);
        }
        engine.render(output);
        Trace trace;
        CHECK(engine.poll(trace) && trace.late_events == 0 && trace.render.clipped == 0);
        for (const auto v : output) CHECK(std::isfinite(v) && std::abs(v) < .1F);
    }
}
}

int main() {
    for (const auto rate : {44100U, 48000U, 96000U}) {
        check_filters(rate, 31, 2800, .707);
        check_filters(rate, 259, 2800, 1.8);
        check_filters(rate, 1301, 6400, 1.2);
        check_envelopes(rate);
    }
    check_cutoff_sweep();
    const auto reduction = 20 * std::log10(largest_jump(0) / largest_jump(64));
    CHECK(std::abs(reduction - 36.12359948) < 1e-6);
    std::cout << "27 complex filter responses and follower, compressor, gate steps at three rates\n"
              << "64-frame automation reduced the largest sample discontinuity by " << reduction << " dB\n";
}
