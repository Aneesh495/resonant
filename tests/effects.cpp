#include "resonant/graph.hpp"
#include "resonant/vm.hpp"
#include "test.hpp"

#include <cmath>

using namespace resonant;

int main() {
    const auto aligned = compile("x=noise(); l=align(x,64); r=convolve_short(x,0); out(l,r);");
    CHECK(aligned.program.latency()[0] == 64 && aligned.program.latency()[1] == 64);
    Vm alignment(aligned.program, 48000);
    std::array<float, 1024> samples{};
    (void)alignment.render(samples);
    for (std::size_t i = 0; i < samples.size()/2; ++i) {
        CHECK(samples[2*i] == samples[2*i+1]);
        if (i < 64) CHECK(samples[2*i] == 0);
    }

    const auto limiter = compile("param u=0; s=audio(u); lim=limiter(s,.25,.15); out(lim);");
    Vm capped(limiter.program, 48000);
    CHECK(capped.event({0, EventType::parameter, 0, 0, name_key("u"), 1.25F}));
    (void)capped.render(std::span(samples).first(2));
    CHECK(samples[0] == 0);
    CHECK(capped.event({1, EventType::parameter, 0, 0, name_key("u"), 0}));
    (void)capped.render(samples);
    CHECK(limiter.program.latency()[0] == 64 && samples[126] == 0.25F);
    for (std::size_t i = 0; i < samples.size()/2; ++i) if (i != 63) CHECK(samples[2*i] == 0);

    const auto oversampled = compile("s=audio(.2); a=oversaturate(s,2); b=align(s,31); out(a,b);");
    CHECK(oversampled.program.latency()[0] == 31 && oversampled.program.latency()[1] == 31);
    Vm warmer(oversampled.program, 96000);
    (void)warmer.render(samples);
    CHECK(std::abs(samples[1022] - (0.4F / 1.4F)) < 3e-6F && samples.back() == 0.2F);

    const auto hq = compile("s=audio(.2); a=oversaturate_hq(s,2); b=align(s,112); out(a,b);");
    CHECK(hq.program.latency()[0] == 112 && hq.program.latency()[1] == 112);
    Vm hq_vm(hq.program, 48000);
    (void)hq_vm.render(samples);
    CHECK(std::abs(samples[1022] - (0.4F / 1.4F)) < 3e-6F && samples.back() == 0.2F);

    const auto room = compile("param click=0; x=audio(click); tank=room(x,.85,1); out(tank);");
    Vm tank(room.program, 48000);
    CHECK(tank.event({0, EventType::parameter, 0, 0, name_key("click"), 0.5F}));
    (void)tank.render(std::span(samples).first(2));
    CHECK(tank.event({1, EventType::parameter, 0, 0, name_key("click"), 0}));
    bool heard = false;
    for (int block = 0; block < 100; ++block) {
        (void)tank.render(samples);
        for (const float x : samples) {
            CHECK(std::isfinite(x) && std::abs(x) <= 0.5F);
            heard |= std::abs(x) > 0.01F;
        }
    }
    CHECK(heard);
}
