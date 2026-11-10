#include "resonant/bytecode.hpp"
#include "resonant/vm.hpp"
#include "test.hpp"

#include <random>

using namespace resonant;

namespace {
Program multi_state(std::uint32_t first, std::uint32_t alignment, std::uint32_t second, float level) {
    return {6, {{1, Op::delay_read, first}, {2, Op::align, alignment}, {3, Op::delay_read, second}},
            {{41, level}},
            {{Op::parameter, Rate::control, 0},
             {Op::audio, Rate::audio, 1, {0}},
             {Op::delay_read, Rate::audio, 2},
             {Op::align, Rate::audio, 3, {2}, 1},
             {Op::delay_read, Rate::audio, 4, {}, 2},
             {Op::add, Rate::audio, 5, {3, 4}},
             {Op::delay_write, Rate::audio, 0, {1}},
             {Op::delay_write, Rate::audio, 0, {2}, 2},
             {Op::output, Rate::audio, 0, {3, 5}}}};
}

void check_mixed_state() {
    std::mt19937 rng(0x543210);
    for (unsigned i = 0; i < 50000; ++i) {
        const auto first = 1U + static_cast<std::uint32_t>(rng() % 31);
        const auto alignment = 1U + static_cast<std::uint32_t>(rng() % 31);
        const auto second = 1U + static_cast<std::uint32_t>(rng() % 31);
        const auto level = static_cast<float>(rng() % 32) / 128;
        auto graph = multi_state(first, alignment, second, level);
        const bool valid = i % 2 == 0;
        if (valid) {
            const auto verified = verify(graph);
            CHECK(verified.work_per_frame() == 37 && verified.state_words() == first + alignment + second);
            const auto bytes = encode(verified);
            auto decoded = decode(bytes);
            CHECK(encode(decoded) == bytes);
            if (i < 100) {
                Vm vm(decoded, 48000);
                std::array<float, 144> out{};
                const auto stats = vm.render(out);
                CHECK(stats.instructions == 9 * out.size() / 2 && stats.clipped == 0);
                for (std::size_t f = 0; f < out.size()/2; ++f) {
                    const auto left = f >= first + alignment ? level : 0.F;
                    const auto extra = f >= first + second ? level : 0.F;
                    CHECK(out[2*f] == left && out[2*f+1] == left + extra);
                }
            }
            continue;
        }
        switch (rng() % 8) {
        case 0: graph.states[0].words = 0; break;
        case 1: graph.states[1].words = max_delay + 1; break;
        case 2: graph.states[1].key = graph.states[2].key; break;
        case 3: graph.instructions[7].slot = 0; break; // Two writes to one delay.
        case 4: graph.instructions[7].slot = 1; break; // Align is not a feedback delay.
        case 5: graph.instructions[3].in[0] = 0; break; // Control wired into audio-only input.
        case 6: graph.instructions[5].dst = 3; break; // Output aliases a live source.
        case 7: std::swap(graph.instructions[5], graph.instructions[6]); break; // Work after delay write.
        }
        rejects([&] { (void)verify(std::move(graph)); });
    }

    auto large = multi_state(max_delay, max_delay, max_delay, .25F);
    for (std::uint64_t key = 4; key <= 9; ++key) large.states.push_back({key, Op::align, max_delay});
    rejects([&] { (void)verify(large); }); // Nine legal extents exceed the aggregate state budget.

    Program heavy{3, {}, {},
                  {{Op::literal, Rate::control, 0, {}, 0, .25F}, {Op::audio, Rate::audio, 1, {0}}}};
    for (std::uint16_t i = 0; i < 3; ++i) {
        heavy.states.push_back({i + 1ULL, Op::convolve_long, 16384});
        heavy.instructions.push_back({Op::convolve_long, Rate::audio, 2, {1, 0}, i});
    }
    heavy.instructions.push_back({Op::output, Rate::audio, 0, {2, 2}});
    CHECK(verify(heavy).work_per_frame() == 126006);
    heavy.states.push_back({4, Op::convolve_short, 8192});
    heavy.instructions.insert(heavy.instructions.end()-1, {Op::convolve_short, Rate::audio, 2, {1, 0}, 3});
    rejects([&] { (void)verify(std::move(heavy)); }); // Valid independent operations exceed the work budget.
}
}

int main() {
    Program base{
        2, {}, {},
        {{Op::literal, Rate::control, 0, {}, 0, 0.25F},
         {Op::audio, Rate::audio, 1, {0}},
         {Op::output, Rate::audio, 0, {1, 1}}}
    };
    const auto good = verify(base);
    CHECK(good.work_per_frame() == 6 && good.state_words() == 0);
    auto bytes = encode(good);
    CHECK(bytes.size() == 68);
    CHECK(bytes[0] == std::byte{'R'} && bytes[6] == std::byte{2});
    CHECK(encode(decode(bytes)) == bytes);
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        rejects([&] { (void)decode(std::span(bytes).first(i)); });
    }
    auto alias = base;
    alias.instructions[1].dst = 0;
    rejects([&] { (void)verify(alias); });
    auto delay = base;
    delay.states = {{5, Op::delay_read, 1}};
    delay.instructions = {{Op::delay_read, Rate::audio, 0}, {Op::delay_write, Rate::audio, 0, {0}},
                          {Op::output, Rate::audio, 0, {0, 0}}};
    CHECK(verify(delay).has_feedback());
    delay.instructions.erase(delay.instructions.begin() + 1);
    rejects([&] { (void)verify(delay); });

    // The fixed independent template defines all legal assignments for these fields.
    // Half the cases retain a legal package, the rest alter one mandatory invariant.
    std::mt19937 rng(0x52534243U);
    for (std::size_t i = 0; i < 1000000; ++i) {
        const float value = static_cast<float>(rng() % 8192) / 8192;
        base.instructions[0].immediate = value;
        auto candidate = base;
        const bool valid = i % 2 == 0;
        if (!valid) {
            switch (rng() % 8) {
            case 0: candidate.instructions[0].op = static_cast<Op>(255); break;
            case 1: candidate.instructions[1].in[0] = 2; break;
            case 2: candidate.instructions[2].in[1] = 0; break;
            case 3: candidate.instructions[2].dst = 1; break;
            case 4: candidate.instructions[0].rate = Rate::audio; break;
            case 5: candidate.instructions[0].slot = 1; break;
            case 6: candidate.instructions[1].in[2] = 1; break;
            case 7: candidate.instructions[2].op = Op::delay_write; break;
            }
        }
        if (valid) {
            const auto loaded = verify(std::move(candidate));
            CHECK(loaded.program().instructions[0].immediate == value);
        } else {
            rejects([&] { (void)verify(std::move(candidate)); });
        }
    }
    check_mixed_state();
    std::cout << "1000000 fixed-template and 50000 mixed-state programs with independent expectations\n";
}
