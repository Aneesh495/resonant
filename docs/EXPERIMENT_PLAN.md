# Experiment protocol v1

Freeze the executable source commit, the input bytes and the native toolchain before each campaign. A successful diagnostic records only the outcomes below. Full acceptance requires a separate evidence bundle with the full durations, actual device and workload, and all coverage rows present.

| Question | Unit and decision rule |
|---|---|
| Can PCM conversion round-trip exact signed bytes? | Use five encoding families and 25,000 independently derived values each, seeded at 0x12def. Check every decoded sample and the integer payload against its reference bytes. Reject an altered RIFF length for every case. For 100,000 seeded dither samples, each outer PCM16 bin must contain 12,000 through 13,000 samples. |
| Does lowering preserve typed graph values? | Generate 10,000 acyclic small arithmetic graphs from seed 0x7342, including forward declarations, both rate classes, parameter changes, folds and dead declarations. Compare the independently computed float values for both output lanes. Separate explicit-delay and bad-type fixtures exercise state and rejection. |
| Does bytecode reject known unsafe structure? | Exercise one million fixed-template valid and invalid cases from seed 0x52534243. Report the diversity limit. Fuzz raw containers and real compiled patches separately. Verify every unsafe operand case before replay. |
| Is FFT convolution time aligned? | Compare four FFT sizes against double direct DFT at absolute error below 2e-5 and five partition sizes against direct FIR below 2e-6. Verify dry path latency exactly. |
| Do the static oscillators meet spectral goals? | Use coherent 4,096-sample tails at bins 19, 233 and 773 at 44.1, 48 and 96 kHz. Measure RMS energy outside integer harmonics relative to harmonic energy. Report all bins. The threshold is -60 dBc for each case. Measure the 2× nonlinear path at all three bins with drive 4, preserving misses. This test leaves modulation sidebands unclassified. |
| Are ramps and SIMD deterministic within one binary? | Compare scalar and block renders with exact PCM equality at 32, 64, 128, 256 and 512 frames at 44.1, 48 and 96 kHz. Use the scored tempo probe and the direct recurrence fixtures. |
| Does live replacement preserve bounded state? | Run 100,000 acknowledged swaps on distinct control and render threads using seed 0x1fbed3, variable block widths, compatible voice state and alternating short delay layouts. Require monotonic generation/frame accounting and finite output. Run the allocation guard separately under the same entry point. |
| Is the reference instrument fast enough on hardware? | Use Apple M2 and the named stereo Core Audio output at exactly 48 kHz and 128 frames. The patch is reference-64.rpatch. Run 64 active voices, two filters, two 8-partition convolvers and two 64-frame limiters. The full threshold is p99.9 below 1.8667 ms and zero engine-attributable deadline misses for eight elapsed hours. Report device underflows separately. |

## Profiles

The diagnostic uses the standard build, the cases above, a short muted device run when available, the native smoke when available, and a gallery export. Maximum diagnostic subprocess time is 180 seconds per unit. Live hardware diagnostics are optional and individually labeled. A one-shot probe verifies the exact program, PCM frames, independently timed score events and bounded replay. Rehashed negative controls alter each relationship separately.

The full profile additionally needs a pinned device identifier, eight elapsed hours on device and another eight elapsed hours offline, callback-wide allocation, deallocation, lock and blocking I/O instrumentation, stable memory and drift measurements, fault and recovery injections, and clean hosted publication. The direct offline stress must cover graph replacement, MIDI bursts, rate changes and cancellation. Device reconfiguration and rate negotiation are separately measured. Physical loopback latency requires loopback hardware.

Record the OS, compiler, CPU, PortAudio version, sample rate and actual block extrema. Hardware deadlines are soft real-time observations. Compare performance claims only when host and workload match. Preserve logs for every failed unit, including instrumented runs and aborted runs. Aggregate results cannot replace per-unit observations.

## Spectral protocol v2

Keep the v1 rates, bins, drive, threshold and source waveform. Preserve all v1 baseline cases alongside a new 8× implementation using cascaded half-band stages. Evaluate the high-quality path under the original -60 dBc threshold. The 2× cases remain labeled baseline and retain their measured failures.

## Spectral protocol v3

Retain the v2 inputs and decisions. The high-quality implementation replaces the last decimation stage with a 129-tap guard-band low-pass at 0.225 cycles per intermediate-rate sample. This sacrifices part of the upper passband to reject harmonics immediately beyond native Nyquist. The [v2 pilot](results/spectral-pilot-v2.csv) retains the results from the half-band-only design.
