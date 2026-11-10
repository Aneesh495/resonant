#include "resonant/graph.hpp"
#include "resonant/session.hpp"
#include "resonant/wave.hpp"

#include <cstdlib>

using namespace resonant;

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size < 1) return 0;
    const auto bytes = std::as_bytes(std::span(data + 1, size - 1));
    const std::string_view text(reinterpret_cast<const char*>(data + 1), size - 1);
    try {
        switch (data[0] % 6) {
        case 0: {
            const auto wave = parse_wave(bytes);
            if (wave.frames() != 0) {
                (void)wave.sample(0, 0);
                (void)wave.sample(wave.frames() - 1, wave.channels - 1);
            }
            break;
        }
        case 1: {
            const auto compiled = compile(text);
            const auto encoded = encode(compiled.program);
            if (encode(decode(encoded)) != encoded) std::abort();
            break;
        }
        case 2: (void)decode(bytes); break;
        case 3: {
            const auto session = parse_session(text);
            const auto saved = encode_session(session);
            if (encode_session(parse_session(saved)) != saved) std::abort();
            break;
        }
        case 4: {
            std::string graph = "gain=.5; n=sine(440); x=mul(n,gain); out(x);";
            for (std::size_t i = 1; i + 1 < size && i < 256; i += 2) {
                graph[data[i] % graph.size()] = static_cast<char>(data[i+1]);
            }
            (void)compile(graph);
            break;
        }
        case 5: {
            const bool assets = size >= 2 && (data[1] & 1U) != 0;
            static const auto plain = encode(compile("param g=.4; a=poly_sine(.1,.3); b=mul(a,g); out(b);").program);
            static const auto embedded = encode(compile("impulse t=pcm(48000,.5,-.25,.125); a=audio(1); "
                                                         "b=convolve_ir(a,1,t); out(b);").program);
            auto encoded = assets ? embedded : plain;
            for (std::size_t i = 1; i + 1 < size && i < 256; i += 2) {
                encoded[data[i] % encoded.size()] = static_cast<std::byte>(data[i+1]);
            }
            (void)decode(encoded);
            break;
        }
        }
    } catch (const std::runtime_error&) {
        // Invalid inputs are expected to stop at the parser or verifier.
    }
    return 0;
}
