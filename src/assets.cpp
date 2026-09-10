#include "resonant/graph.hpp"
#include "resonant/wave.hpp"

#include <algorithm>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>

namespace resonant {
namespace {
std::string declaration(std::string_view name, const WaveView& wave, bool table) {
    const auto letter = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; };
    if (name.empty() || name.size() > 63 || !letter(name.front()) ||
        !std::ranges::all_of(name, [&](char c) { return letter(c) || (c >= '0' && c <= '9'); }) ||
        name == "out" || name == "param" || name == "impulse" || name == "table") {
        throw std::runtime_error("asset name must be a non-reserved patch identifier");
    }
    const auto frames = wave.frames();
    if (frames == 0 || frames > max_asset_samples) throw std::runtime_error("asset must contain 1 to 1024 WAV frames");
    const auto mono = map_channels(wave, 1);
    std::vector<float> samples(mono.begin(), mono.end());
    if (table && !valid_wavetable(samples)) {
        throw std::runtime_error("wavetable must have a power-of-two frame count from 16 to 1024 and normalized samples");
    }
    if (!table && !valid_impulse(samples, wave.rate)) {
        throw std::runtime_error("impulse rate or gain exceeds the supported range");
    }
    std::ostringstream text;
    text.imbue(std::locale::classic());
    if (table) text << "table " << name << " = cycle(";
    else text << "impulse " << name << " = pcm(" << wave.rate << ',';
    text << std::setprecision(std::numeric_limits<float>::max_digits10);
    for (std::size_t i = 0; i < samples.size(); ++i) {
        if (i != 0) text << ',';
        text << (i % 12 == 0 ? "\n  " : " ") << samples[i];
    }
    text << ");\n";
    return text.str();
}

}

std::string impulse_declaration(std::string_view name, const WaveView& wave) {
    return declaration(name, wave, false);
}

std::string wavetable_declaration(std::string_view name, const WaveView& wave) {
    return declaration(name, wave, true);
}
}
