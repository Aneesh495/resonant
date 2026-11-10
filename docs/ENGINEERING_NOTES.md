# Engineering notes

1. **PCM.** Integer sample ranges have asymmetric endpoints. The decoder sign-extends before scaling. A malformed alignment byte is a chunk framing error. [Encoding](PCM.md) · [Tests](../tests/wave.cpp)
2. **Graph rates.** Control values can ramp every frame. Rate controls where a signal can connect. It does not imply a particular host block cadence. [Language](SPECIFICATION.md)
3. **Bytecode safety.** Fixed arity and exact record framing bound operand interpretation. State extents and the global work cap apply to the decoded package. [Verifier](../src/bytecode.cpp)
4. **Scheduling.** A deterministic queue of dependency-free vertices establishes order. Deferred delay writes are omitted from the current-frame graph. [Compiler](../src/compiler.cpp)
5. **Aliasing.** Registers become free after the last consumer, including deferred writes and terminal output. SIMD kernels only receive destinations distinct from their source registers. [Register diagram](diagrams/registers.svg)
6. **Callback ownership.** The callback touches preallocated packages and bounded atomics. File I/O, destruction and editor work take place elsewhere. [Protocol](REALTIME.md)
7. **Publication.** Audio switches only at block boundaries. A migration plan names its base generation. A mismatch bypasses copies and still fades. [Lifecycle](diagrams/swap.svg)
8. **MIDI clocks.** A score integrates tempo as integer tick-microseconds, then rounds to the sample grid once. A live byte stream inherits its supplied frame timestamp. Tied note-off events precede note-on in scores. [Timing](../src/session.cpp)
9. **Synthesis aliasing.** Harmonic table size, interpolation and phase-step limits define static spectral behavior. Fast modulation has a different sideband model. [Spectral protocol](EXPERIMENT_PLAN.md)
10. **Filter stability.** The topology-preserving SVF uses positive damping. Parameter bounds keep individual coefficient sets supported. The nonlinear and state paths also bound exceptional values. [DSP contracts](DSP.md)
11. **Convolution latency.** P input frames are needed to complete one FFT hop. Wet and dry are both delayed by P. Long histories reset on incompatible graph replacement. [Hop diagram](diagrams/convolution.svg)
12. **Dynamics.** The lookahead limiter bounds sample peaks. Intersample peaks and playback reconstruction can exceed the discrete sample bound. [Dynamics implementation](../src/effects.cpp)
13. **SIMD precision.** Scalar and block use binary32 signal lanes, no fast math and disabled contraction in supported builds. The arithmetic SIMD kernels flush tiny intermediates to the same explicit floor. [SIMD kernels](../src/simd.cpp)
14. **Measured deadlines.** The engine duration omits host callback entry and trace-ring publication. Device flags are preserved separately. A general-purpose OS can delay a callback even when its execution cost fits the deadline. [Protocol](EXPERIMENT_PLAN.md)

## Walkthroughs

- Compile [the reference patch](../patches/reference-64.rpatch) with `build/resonant inspect`. Follow one literal from [parser](../src/parser.cpp) to [register allocation](../src/compiler.cpp) and [verified execution](../src/vm.cpp).
- Render the [tempo probe](../tests/fixtures/tempo-probe.rsn). Follow the note-off at tick 1139 through [scheduling](../src/session.cpp), [event delivery](../src/engine.cpp) and [independent verification](../tools/verify_evidence.py).
- Connect a port in the [workbench](../src/apple/workbench.mm). The edited source is compiled, posted and faded at a callback boundary. [Swap and reclamation](../src/engine.cpp)
- Introduce a cycle without a delay. The compiler returns a spanned error and the previous generation continues. [Recovery diagram](diagrams/recovery.svg) · [Cycle tests](../tests/compiler.cpp)
