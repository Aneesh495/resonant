#include "resonant/graph.hpp"
#include "resonant/wave.hpp"

#include <algorithm>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>

namespace resonant {
std::string impulse_declaration(std::string_view name, const WaveView& wave) {
    const auto letter = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; };
    if (name.empty() || name.size() > 63 || !letter(name.front()) ||
        !std::ranges::all_of(name, [&](char c) { return letter(c) || (c >= '0' && c <= '9'); }) ||
        name == "out" || name == "param" || name == "impulse") {
        throw std::runtime_error("impulse name must be a non-reserved patch identifier");
    }
    const auto frames = wave.frames();
    if (frames == 0 || frames > max_impulse_samples) throw std::runtime_error("impulse must contain 1 to 1024 WAV frames");
    const auto mono = map_channels(wave, 1);
    std::vector<float> samples(mono.begin(), mono.end());
    if (!valid_impulse(samples, wave.rate)) throw std::runtime_error("impulse rate or gain exceeds the supported range");
    std::ostringstream text;
    text.imbue(std::locale::classic());
    text << "impulse " << name << " = pcm(" << wave.rate << std::setprecision(std::numeric_limits<float>::max_digits10);
    for (std::size_t i = 0; i < samples.size(); ++i) {
        text << (i % 12 == 0 ? ",\n  " : ", ") << samples[i];
    }
    text << ");\n";
    return text.str();
}
}
