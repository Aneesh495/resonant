#include "resonant/graph.hpp"
#include "resonant/session.hpp"
#include "resonant/wave.hpp"
#include "test.hpp"

#include <algorithm>
#include <cmath>

using namespace resonant;

int main() {
    const std::array<double, 6> channels{.75,.25, -.25,-.25, 0,.25};
    const auto wav = encode_wave(channels, {Encoding::pcm24, 48000, 2});
    const auto wave = parse_wave(wav);
    const auto asset = impulse_declaration("plate", wave);
    const std::string patch = asset + "param p=0; a=audio(p); b=convolve_ir(a,1,plate); out(b);";
    const auto graph = parse_graph(patch);
    CHECK(graph.impulses.size() == 1 && graph.impulses[0].rate == 48000);
    const std::array<float, 3> expected{.5F, -.25F, .125F};
    CHECK(std::ranges::equal(graph.impulses[0].samples, expected));
    const auto compiled = compile(patch);
    const auto bytes = encode(compiled.program);
    CHECK(bytes[4] == std::byte{2} && format_version(compiled.program) == 2);
    CHECK(format_version(compile("a=audio(1); out(a);").program) == 1);
    const auto decoded = decode(bytes);
    CHECK(encode(decoded) == bytes && decoded.latency()[0] == 128);
    CHECK(decoded.program().states.front().asset_rate == 48000);
    CHECK(std::ranges::equal(decoded.program().states.front().impulse, expected));

    // An impulse reaches the convolver at sample zero. Each output tap appears 128 frames later.
    Timeline timeline{256, {{0, EventType::parameter, 0, 0, name_key("p"), 1},
                            {1, EventType::parameter, 0, 0, name_key("p"), 0}}};
    for (const auto rate : {44100U, 48000U, 96000U}) {
        const auto rendered = render_offline(decoded, timeline, rate, 64);
        for (std::size_t i = 0; i < timeline.frames; ++i) {
            const auto value = i >= 128 && i < 131 ? expected[i-128] : 0.F;
            CHECK(std::abs(rendered.samples[2*i] - value) < 1e-6F);
            CHECK(rendered.samples[2*i] == rendered.samples[2*i+1]);
        }
    }
    auto session = Session{};
    session.source = patch;
    CHECK(program_key(compile(parse_session(encode_session(session)).source).program) == program_key(decoded));

    auto changed = bytes;
    changed.back() = std::byte{0x7f}; // Invalid IEEE float tap.
    rejects([&] { (void)decode(changed); });
    changed = bytes;
    changed[4] = std::byte{1};
    rejects([&] { (void)decode(changed); });
    changed = bytes;
    changed.push_back(std::byte{0});
    rejects([&] { (void)decode(changed); });
    for (std::size_t trim = 1; trim <= 20; ++trim) {
        rejects([&] { (void)decode(std::span(bytes).first(bytes.size() - trim)); });
    }
    rejects([&] { (void)impulse_declaration("x;out", wave); });
    rejects([&] { (void)compile("impulse p=pcm(48000,1.1); a=audio(0); out(a);"); });
    rejects([&] { (void)compile("impulse a=pcm(48000,.1); a=audio(0); out(a);"); });
    rejects([&] { (void)compile("a=audio(0); b=convolve_ir(a,1,a); out(b);"); });
}
