# Build status

Resonant has a runnable instrument, a native macOS workbench and a portable offline CLI. The full profile remains incomplete.

| Area | Implemented and exercised | Remaining scope |
|---|---|---|
| PCM and files | Signed and floating WAVE, dither histogram, clipping, frame counts, channel mapping, session v1 migration, atomic saves and portable embedded WAV impulse import | More third-party RIFF fixtures |
| Graph and verifier | Typed rates, source diagnostics, deterministic scheduling, folding, physical register reuse, delayed cycles, independent structural verifier | Broader generated graph and bytecode families, particularly multi-state adversarial mixtures |
| Engine | Scalar, NEON/SSE2 arithmetic, bounded queues, timestamped ramps, transport cancellation, live replacement and short compatible-state migration | Callback-wide lock and blocking I/O instrumentation, denormal and floating-point mode audit on additional platforms |
| DSP | Band-limited tables, modulation, voices, filters, fractional delays, algorithmic room, convolution, compressor, limiter and oversampling | Physical and modulated spectral measurements, external wavetable import, automatic latency compensation |
| Device and MIDI | PortAudio enumeration, stereo open/stop/reconfigure and muted smoke. Live MIDI 1.0 input and host clock mapping tested through a Core MIDI virtual source and muted hardware output | Physical MIDI controller and DAC loopback measurements, full device fault campaign |
| Native workbench | Port editing, source and schedule, score, parameters, scope/spectrum, capture, tempo, save/reopen and WAV export | Full interactive acceptance across machines and accessibility workflows |
| Evidence | Source-bound diagnostic, independent bytecode and timeline checks, bounded replay and rehashed negative controls | Full independent offline and device eight-hour campaigns, complete release acceptance |

The 2× saturation baseline misses the spectral threshold at several test tones. The 8× path with a guard-band decimator meets the threshold on the frozen static test grid. The result does not cover arbitrary modulation. Offline publication stress and a short muted device run do not certify deadline or lifetime behavior for eight hours.

The `full` acceptance entry point emits a blocked report and returns nonzero until the missing campaign and instrumentation are available. The diagnostic has a separate status and bundle.
