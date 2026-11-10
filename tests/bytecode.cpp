#include "resonant/bytecode.hpp"
#include "test.hpp"

#include <random>

using namespace resonant;

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
    std::cout << "1000000 independently labeled fixed-template programs\n";
}
