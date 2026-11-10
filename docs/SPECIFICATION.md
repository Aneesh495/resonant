# Patch and bytecode contract

Patch identifiers are ASCII and case-sensitive. A statement ends in a semicolon. A hash begins a line comment. Declarations can refer forward to named mono values. A patch has one `out(left, right)` or `out(mono)` statement. The latter duplicates its input into the two device lanes.

```text
param pitch = 220;
dry = sine(pitch);
wet = delay(mix, 9000);
echo = mul(wet, 0.45);
mix = add(dry, echo);
soft = mul(mix, 0.12);
out(soft);
```

Literals and parameters are control signals. Arithmetic joins the input rates, and `audio(x)` explicitly promotes a value. Oscillators and noise produce audio. Filters, gain effects, delay input and output require audio in their signal port. Modulation ports accept either rate. Control values can vary within a block because automation follows the sample clock.

The compiler validates the complete declared graph, sorts the current-sample dependencies, folds literal arithmetic, and removes everything unreachable from output. A delay read depends on prior state. Its write happens after all current-sample calculations. All other cycles fail. Each compiler row retains the source byte span and logical dependencies. Register allocation releases a temporary after its final use, including deferred writes and output.

## Wire format v1

All fields are little-endian. The 20-byte header is `RBSC`, version u16, registers u16, states u16, parameters u16, instructions u32, zero u32. Records are 16 bytes. A state is key u64, owner u32, extent u32. A parameter is key u64, initial binary32, zero u32. An instruction is opcode u8, rate u8, destination u16, three input u16s, slot u16 and immediate binary32. The declaration order in [bytecode.hpp](../include/resonant/bytecode.hpp) determines opcode ordinals. The [opcode table](../src/bytecode.cpp) supplies arity, rate rule, state size and conservative work units.

The verifier tracks the current rate and initialization of every physical register. It rejects an instruction overwriting one of its own sources. It verifies a unique owner for each state, a unique write for each delay, one terminal stereo output, and a contiguous delay commit section. State extents and work units accumulate under global caps. No instruction changes the program counter. Each frame executes at most the verified instruction count. A delay extent bounds its ring cursor. Work units are a static model and require separate timing evidence.
