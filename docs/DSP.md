# DSP contracts

Oscillator phase is an unsigned Q0.32 accumulator. The sine table uses 4,096 samples and linear interpolation. Saw and triangle tables contain bounded Fourier partials. The oscillator chooses a conservative pair of harmonic bands based on phase increment and crossfades within the octave. Pulse subtracts two phase-shifted saws and adds a duty-dependent DC term. Fast pitch, duty or phase changes create sidebands outside the static-tone model. Noise uses a per-node deterministic xorshift sequence.

A polyphonic node allocates 64 voices. A same-channel same-pitch retrigger preserves phase and envelope level. An idle slot wins before stealing. Among active voices, release has priority, followed by the smallest envelope and oldest event. Attack and release have positive sample bounds. Sustain delays release until pedal up. A hard panic clears all active voices.

The state-variable filter uses a topology-preserving integrator update. Cutoff is clamped to 0.0001 through 0.45 of sample rate. Q is clamped to 0.25 through 12. Coefficients are evaluated from current signal parameters. Ramping the cutoff interpolates the parameter, which keeps each instantaneous coefficient set in the supported region. This does not prove stability under arbitrary modulation.

The fractional delay uses linear interpolation and clamps feedback to magnitude 0.97. The all-pass uses that delay with feed-forward cancellation at fixed delay and gain. The algorithmic room feeds four damped combs into two all-pass stages. The delay lengths are derived from the sample rate. Its dry path is immediate. Feedback coefficients stay below unity.

The partitioned convolution nodes use complex radix-2 FFTs and frequency-domain delay lines. The short and long presets generate original impulses of 512 and 1,024 taps with 64- and 128-frame partitions. Each hop transforms one input block and one accumulated output spectrum. The inverse transform's second half overlaps the next block. Dry and wet share the partition delay. Switching a preset uses graph replacement and a crossfade. Convolution state exceeds the migration copy budget and starts with an empty history.

The oversampled saturator inserts zeros at 2×, interpolates with a 63-tap Blackman-windowed half-band FIR, applies the bounded rational transfer, filters again and decimates. Nominal group delay is 31 native frames. The transfer is bounded under any verified signal amplitude. The FIR does not remove alias already produced above the 2× Nyquist limit.

The limiter keeps a 64-frame lookahead and scans the delayed samples and current input. It applies immediate gain reduction and a release envelope. Its guarantee covers discrete sample peaks. The separate compressor uses a fixed 4:1 ratio and has no lookahead. The gate and follower expose envelope time controls.

The verifier reports left and right algorithmic path latency. Add `align(signal, frames)` to compensate a parallel dry branch. Echo and tank tails are intentional and excluded from algorithmic latency. The report gives the maximum explicit latency along each output path. A mixed path can have multiple arrival times.
