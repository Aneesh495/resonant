# Architecture decisions

## 1. Linear bytecode

The language lowers to a fixed instruction stream. A branch-capable VM would require validating control-flow and proving an iteration budget. A linear stream gives every callback a finite instruction count and makes source-to-schedule inspection direct.

## 2. Separate verification boundary

The compiler emits ordinary records. The verifier reads their current register definitions, rate rules and state owners before a VM can allocate state. The compiler's dependency graph is unavailable to the verifier, so a corrupt package cannot borrow compiler invariants.

## 3. Mono registers and explicit stereo output

Stereo buses are pairs of mono values. This preserves simple lane and liveness rules and makes pan a visible operation. A graph-wide multichannel shape system would support broader routings but add port and alias cases.

## 4. Feedback commit epilogue

Delay reads expose old state, then commit after the combinational schedule. Any algebraic cycle is rejected. A frame-major path for explicit feedback costs more than the block kernels and preserves one-frame timing.

## 5. Physical register reuse

Allocating a buffer per declaration simplifies debug but enlarges working sets. Release after final use keeps memory bounded. A source-linked schedule records reuse and the verifier forbids same-instruction aliasing.

## 6. Fixed event budget

A single producer provides monotonic frame order. The callback handles at most 128 eligible events. An over-budget event remains queued and is visibly late when consumed. The queue also exposes rejected entries and cancellation.

## 7. One-item edit mailbox

A burst of edits often has intermediate states that the musician never needs to hear. The mailbox coalesces unconsumed edits and reports the count. An independent retirement mailbox gives the audio thread an ownership handoff without reference counting.

## 8. Bounded migration

Matching names, kinds, extents and sample rates are sufficient for selected state copies. Copying entire impulse histories on audio would cost too much. The 2,048-word budget resets larger state and uses the same short crossfade as incompatible graph edits.

## 9. Harmonic tables

Conservative harmonic tables trade memory for predictable voice cost. Linear interpolation needs fewer operations than per-sample additive synthesis. Static spectra are measured separately from time-varying modulation.

## 10. Partitioned FFT convolution

A direct FIR provides the independent reference. Uniform partitions trade one partition of latency for lower cost on longer impulses. A bounded ring of input spectra reuses the same transform buffers at every hop.

## 11. Host adapter outside the runtime

PortAudio provides device access and reports host stream status. It does not compile, schedule or synthesize. The offline renderer can reuse the runtime without a device, and unsupported device requests return errors.

## 12. Native controls over a shared patch

The AppKit view changes the source span of an input and then runs the real compiler and publication path. A parallel editor-only graph model would drift from executed behavior. The native editor therefore keeps the source as the authoritative patch.
