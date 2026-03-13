#include "resonant/graph.hpp"
#include "test.hpp"

using namespace resonant;

int main() {
    const std::string source = R"(
out(scaled);
scaled = mul(sinewave, level);
sinewave = sine(hertz);
hertz = add(220, 220);
level = mul(0.5, 0.5);
never = noise();
)";
    const auto c = compile(source);
    const auto& program = c.program.program();
    CHECK(c.folded == 2 && c.eliminated == 3);
    CHECK(program.registers <= 3);
    CHECK(program.states.size() == 1 && program.states[0].key == name_key("sinewave"));
    CHECK(c.schedule.size() == program.instructions.size());
    CHECK(c.schedule.back().dependencies[0] == "scaled");
    CHECK(encode(c.program) == encode(compile(source).program));

    const auto delayed = compile("out(mix); memory=delay(mix,1); mix=add(memory,1);");
    CHECK(delayed.program.has_feedback() && delayed.program.state_words() == 1);
    CHECK(delayed.program.program().instructions[delayed.schedule.size()-2].op == Op::delay_write);
    CHECK(delayed.schedule.front().dependencies[0] == "mix");

    for (const auto invalid : {"out(a); a=add(b,1); b=mul(a,2);",
                               "out(a); a=audio(1); z=add(z,1);",
                               "out(c); c=add(1,2);",
                               "out(l,r); l=audio(1); r=1;",
                               "out(a); a=delay(1,10);",
                               "out(a); a=delay(a,0);",
                               "out(a); a=delay(a,1.5);",
                               "out(a); a=audio(0); a=noise();",
                               "out(a); a=sine(x);",
                               "out(a); a=lowpass(1,30,1);",
                               "out(a); a=delay_read();"}) {
        rejects([&] { (void)compile(invalid); });
    }
    std::string long_chain = "param n=0.1; r0=audio(n);";
    for (int i = 1; i < 450; ++i) {
        long_chain += "r" + std::to_string(i) + "=add(r" + std::to_string(i-1) + ",0.001);";
    }
    long_chain += "out(r449);";
    CHECK(compile(long_chain).program.program().registers <= 3);
}
