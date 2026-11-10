# Reproduce a diagnostic

Use a clean checkout with a C++23 compiler, CMake 3.25+ and Python 3.11+. The Linux runner builds without PortAudio and exercises the real offline engine. The macOS device run needs stereo Core Audio output and PortAudio 19.7.

```sh
make doctor
make build
make acceptance PROFILE=diagnostic RUN_ID=sample EXTRA_PROFILE_ARGS=--gallery
make verify RUN_ID=sample
make negative-controls RUN_ID=sample
```

Add `--device` alongside `--gallery` on macOS to run the short muted hardware workload and native workbench smoke. Every run ID must be new. The bundle has an input SHA-256 manifest, toolchain and environment records, command exit statuses, exact event tape, probe PCM, spectral fixtures and parity renders. The capture proof also stores screenshots and the edited session. The verifier checks the manifest against a trusted commit. It independently derives score timing and spectral residuals, then recompiles and replays the probe with the trusted local executable. Replay uses exact PCM within a compatible host and toolchain. Cross-platform elementary math may require separate same-platform evidence.

Run `make fuzz` with Clang and libFuzzer for the ASan/UBSan parser and verifier diagnostic. When Apple Command Line Tools omit the fuzz runtime, select an LLVM build that provides it with `FUZZ_BUILD=build-fuzz-llvm FUZZ_CMAKE_ARGS=-DCMAKE_CXX_COMPILER=/path/to/clang++`. Run `make realtime-audit` for a C++ heap allocation/deallocation guard on the engine render entry point. It does not observe every C allocation or blocking operation in the device host stack.

`make bench SECONDS=10` retains a raw timing trace under `out/`. `make device-soak` is a standalone muted static reference workload with a raw trace. It does not exercise the remaining full-profile fault and reconfiguration campaign. Host-reported output latency is distinct from physical loopback.

`make release` tests, runs the offline demo and stages the CLI, bundled native app when enabled, patches and scores in `out/stage`.

## Recovery

A failed compile leaves the current audio package active. Correct the first source error and apply again. A failed or disconnected device can be stopped and reopened from the device menu. A stopped transport clears pending events at the next callback boundary. Captures with discontinuities are rejected. Interrupted session writes leave the last committed file in place, with a temporary sibling possible after process termination. Cancel an export before its final file rename to discard the new render.
