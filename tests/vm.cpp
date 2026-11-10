#include "resonant/graph.hpp"
#include "resonant/vm.hpp"
#include "test.hpp"

#include <numbers>
#include <random>

using namespace resonant;

int main() {
    const auto sine = compile("tone=sine(997); out(tone);");
    Vm vm(sine.program, 48000);
    std::array<float, 1024> audio{};
    const auto stats = vm.render(audio);
    CHECK(stats.instructions == 512 * sine.schedule.size());
    for (std::size_t i = 0; i < audio.size()/2; ++i) {
        CHECK(audio[2*i] == audio[2*i+1]);
        const auto truth = std::sin(2 * std::numbers::pi * 997.0 * static_cast<double>(i) / 48000);
        CHECK(std::abs(audio[2*i] - truth) < 2e-6);
    }

    const auto feedback = compile("d=delay(y,3); x=mul(d,0.5); y=add(x,0.1); out(y,d);");
    Vm echo(feedback.program, 48000);
    (void)echo.render(audio);
    for (std::size_t i = 0; i < audio.size()/2; ++i) {
        const auto prior = i < 3 ? 0 : audio[2*(i-3)];
        CHECK(audio[2*i+1] == prior && audio[2*i] == 0.5F * prior + 0.1F);
    }

    const auto ramp = compile("param value=0; x=audio(value); out(x);");
    Vm automated(ramp.program, 48000);
    CHECK(automated.event({0, EventType::parameter, 0, 0, name_key("value"), 0.5F, 8}));
    (void)automated.render(audio);
    for (std::size_t i = 0; i < 8; ++i) CHECK(audio[2*i] == 0.0625F * static_cast<float>(i+1));
    CHECK(audio.back() == 0.5F);

    const auto voice = compile("p=poly_sine(0.001,0.003); out(p);");
    Vm synth(voice.program, 48000);
    for (std::uint8_t note = 24; note < 88; ++note) CHECK(synth.event({0, EventType::note_on, 0, note, 0, 0.3F}));
    CHECK(synth.render(audio).voices == 64);
    CHECK(synth.event({0, EventType::sustain, 0, 0, 0, 1}));
    for (std::uint8_t note = 24; note < 88; ++note) CHECK(synth.event({0, EventType::note_off, 0, note}));
    CHECK(synth.render(audio).voices == 64);
    CHECK(synth.event({0, EventType::sustain, 0, 0, 0, 0}));
    CHECK(synth.render(audio).voices == 0);

    const auto mixed = compile("p=poly_saw(.002,.2); n=noise(); q=mul(n,.02); d=add(p,q); "
                               "w=lowpass(d,1400,1.2); l=pan_left(w,.3); r=pan_right(w,.3); out(l,r);");
    Vm scalar(mixed.program, 44100, Execution::scalar);
    Vm block(mixed.program, 44100);
    CHECK(scalar.event({0, EventType::note_on, 0, 40, 0, 0.8F}));
    CHECK(block.event({0, EventType::note_on, 0, 40, 0, 0.8F}));
    std::array<float, 1024> reference{};
    std::mt19937 rng(22);
    for (int i = 0; i < 150; ++i) {
        const auto width = 2 * (1 + rng() % 512);
        (void)scalar.render(std::span(reference).first(width));
        (void)block.render(std::span(audio).first(width));
        for (std::size_t j = 0; j < width; ++j) CHECK(audio[j] == reference[j]);
    }
}
