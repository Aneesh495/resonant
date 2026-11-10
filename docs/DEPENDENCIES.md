# Dependencies

The core targets C++23 with CMake 3.25 or later. The system math library provides elementary functions. The engine does not embed an existing synthesis or DSP framework.

The optional device adapter targets PortAudio 19.7 (MIT). The native workbench targets AppKit on macOS. Graphviz is a documentation tool. None of these dependencies supply the graph compiler, scheduling or DSP state machine.

Primary references include the [PortAudio callback guide](https://portaudio.com/docs/v19-doxydocs/writing_a_callback.html), the [PortAudio API overview](https://files.portaudio.com/docs/v19-doxydocs-dev/api_overview.html), the [Microsoft RIFF description](https://learn.microsoft.com/en-us/windows/win32/xaudio2/resource-interchange-file-format--riff-), Julius O. Smith's [Spectral Audio Signal Processing](https://www.dsprelated.com/freebooks/sasp/), and the [MIDI Association specifications](https://midi.org/specifications).
