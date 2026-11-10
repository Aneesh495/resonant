# PCM framing and quantization

The decoder accepts little-endian RIFF with mono or stereo signed PCM16/24/32 or IEEE binary32/binary64. The RIFF size must match the input. Each chunk has a bounded payload and an alignment byte for odd lengths. One format chunk and one data chunk are required. Unknown chunks are skipped. Extended and compressed formats are rejected.

Integer normalization divides the sign-extended value by 2^(bits-1). The encoder rounds to nearest with ties away from zero, clamps to the signed range and writes the low bytes. Positive and negative full scale are asymmetric. The float decoder retains finite samples above unity and substitutes zero for nonfinite samples. The original bytes remain available in the view. Stereo downmix gives each side half weight. Mono expands at unity.

An optional seeded xorshift generator subtracts two independent uniform draws per sample to produce triangular dither in quantizer units. The silence check counts the three resulting PCM16 levels against the theoretical 1:6:1 proportions. It does not characterize audible noise or distortion for arbitrary inputs.

CLI renders attach a RIFF INFO comment carrying the requested frame count, the FNV-1a identity of the score or patch input, and the identity of its verified bytecode. The diagnostic bundle separately binds raw files and sources with SHA-256.

Workbench exports carry the same frame, source and bytecode identity fields as CLI renders. For a live recording, the callback also identifies both programs in each crossfade. The capture comment gives the first frame, total recorded frames, first and last program IDs, and an FNV-1a fingerprint of the recorded blocks and their program contributions. Captures can mix several programs and live input, so this fingerprint identifies provenance rather than a replayable session.
