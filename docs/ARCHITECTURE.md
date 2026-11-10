# Architecture

A patch compiles to a linear instruction stream with a fixed work budget. The loader independently checks operand types, availability, state layouts and cost before creating a runnable program. RIFF/WAVE stores sample data and cannot supply executable instructions.

The audio thread owns mutable DSP state. The control thread prepares complete replacements, posts bounded timestamped events and destroys retired packages. All callback-visible storage is allocated in advance. Device I/O uses PortAudio. Offline rendering uses the same engine. Playback starts muted.

The product scope is interactive synthesis and sequencing. Its PCM and spectral components serve that instrument. Murmur handles offline repair and redaction. Quartz simulates communications. Splice is a shell and Cinder is a general-purpose C compiler.
