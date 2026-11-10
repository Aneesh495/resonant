#include "resonant/wave.hpp"

#include "bytes.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <locale>
#include <sstream>
#include <string_view>

namespace resonant {
namespace {
struct Format {
    std::uint16_t tag;
    std::uint16_t width;
};

Format format(Encoding encoding) {
    switch (encoding) {
    case Encoding::pcm16: return {1, 2};
    case Encoding::pcm24: return {1, 3};
    case Encoding::pcm32: return {1, 4};
    case Encoding::float32: return {3, 4};
    case Encoding::float64: return {3, 8};
    }
    throw std::runtime_error("unsupported sample encoding");
}

Encoding encoding(std::uint16_t tag, std::uint16_t bits) {
    if (tag == 1 && bits == 16) return Encoding::pcm16;
    if (tag == 1 && bits == 24) return Encoding::pcm24;
    if (tag == 1 && bits == 32) return Encoding::pcm32;
    if (tag == 3 && bits == 32) return Encoding::float32;
    if (tag == 3 && bits == 64) return Encoding::float64;
    throw std::runtime_error("unsupported WAVE format tag or sample width");
}

void validate_layout(std::uint16_t channels, std::uint32_t rate) {
    if (channels < 1 || channels > 2) throw std::runtime_error("only mono and stereo are supported");
    if (rate < 1 || rate > 384000) throw std::runtime_error("unsupported WAVE sample rate");
}

bool fourcc(std::span<const std::byte> bytes, std::size_t offset, std::string_view name) {
    if (offset > bytes.size() || name.size() > bytes.size() - offset) return false;
    for (std::size_t i = 0; i < name.size(); ++i) {
        if (bytes[offset + i] != static_cast<std::byte>(name[i])) return false;
    }
    return true;
}

void fourcc(binary::Bytes& bytes, std::string_view name) {
    for (const char c : name) bytes.push_back(static_cast<std::byte>(c));
}

double finite(double x) {
    return std::isfinite(x) ? x : 0.0;
}

double uniform(std::uint32_t& seed) {
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    return static_cast<double>(seed) * 0x1p-32;
}
}

std::size_t WaveView::frames() const {
    validate_layout(channels, rate);
    return payload.size() / (format(encoding).width * channels);
}

double WaveView::sample(std::size_t frame, std::size_t channel) const {
    if (channel >= channels || frame >= frames()) throw std::out_of_range("WAVE sample index");
    const auto type = format(encoding);
    const auto pos = (frame * channels + channel) * type.width;
    if (encoding == Encoding::float32) {
        static_assert(std::numeric_limits<float>::is_iec559 && sizeof(float) == 4);
        return finite(std::bit_cast<float>(binary::read<std::uint32_t>(payload, pos)));
    }
    if (encoding == Encoding::float64) {
        static_assert(std::numeric_limits<double>::is_iec559 && sizeof(double) == 8);
        return finite(std::bit_cast<double>(binary::read<std::uint64_t>(payload, pos)));
    }
    const std::uint32_t raw = binary::read<std::uint32_t>(payload, pos, type.width);
    const std::int64_t half = std::int64_t{1} << (8 * type.width - 1);
    const std::int64_t signed_value = raw >= half ? raw - 2 * half : raw;
    return static_cast<double>(signed_value) / static_cast<double>(half);
}

std::string render_comment(std::uint64_t frames, std::uint64_t source_id, std::uint64_t program_id) {
    std::ostringstream text;
    text.imbue(std::locale::classic());
    text << "Resonant frames=" << frames << " source_id=" << std::hex << source_id << " program_id=" << program_id;
    return text.str();
}

WaveView parse_wave(std::span<const std::byte> bytes) {
    using binary::read;
    if (!fourcc(bytes, 0, "RIFF") || !fourcc(bytes, 8, "WAVE")) {
        throw std::runtime_error("expected a little-endian RIFF/WAVE file");
    }
    if (read<std::uint32_t>(bytes, 4) != bytes.size() - 8) {
        throw std::runtime_error("RIFF length does not match file size");
    }

    WaveView view{};
    bool has_format = false;
    bool has_data = false;
    for (std::size_t pos = 12; pos < bytes.size();) {
        const auto width = read<std::uint32_t>(bytes, pos + 4);
        const std::size_t body = pos + 8;
        const std::size_t padded = static_cast<std::size_t>(width) + (width & 1U);
        if (body > bytes.size() || padded > bytes.size() - body) {
            throw std::runtime_error("truncated WAVE chunk or padding");
        }
        const auto chunk = bytes.subspan(body, width);
        if (fourcc(bytes, pos, "fmt ")) {
            if (has_format || (width != 16 && width != 18)) throw std::runtime_error("invalid fmt chunk");
            if (width == 18 && read<std::uint16_t>(chunk, 16) != 0) {
                throw std::runtime_error("WAVE format extensions are unsupported");
            }
            const auto tag = read<std::uint16_t>(chunk, 0);
            view.channels = read<std::uint16_t>(chunk, 2);
            view.rate = read<std::uint32_t>(chunk, 4);
            view.encoding = encoding(tag, read<std::uint16_t>(chunk, 14));
            validate_layout(view.channels, view.rate);
            const auto alignment = static_cast<std::uint32_t>(view.channels * format(view.encoding).width);
            if (read<std::uint16_t>(chunk, 12) != alignment ||
                read<std::uint32_t>(chunk, 8) != view.rate * alignment) {
                throw std::runtime_error("invalid WAVE block alignment or byte rate");
            }
            has_format = true;
        } else if (fourcc(bytes, pos, "data")) {
            if (has_data) throw std::runtime_error("duplicate data chunk");
            view.payload = chunk;
            has_data = true;
        }
        pos = body + padded;
    }
    if (!has_format || !has_data) throw std::runtime_error("missing fmt or data chunk");
    if (view.payload.size() % (view.channels * format(view.encoding).width) != 0) {
        throw std::runtime_error("partial WAVE frame");
    }
    return view;
}

std::vector<std::byte> encode_wave(std::span<const double> samples, WaveOptions options) {
    using binary::write;
    validate_layout(options.channels, options.rate);
    const auto type = format(options.encoding);
    if (samples.size() % options.channels != 0) throw std::runtime_error("partial input frame");
    if (options.comment.size() > 4096 || options.comment.find('\0') != std::string_view::npos) {
        throw std::runtime_error("invalid WAVE comment");
    }
    const auto comment_size = options.comment.empty() ? 0U : static_cast<std::uint32_t>(options.comment.size() + 1);
    const auto comment_padding = comment_size & 1U;
    const auto extra = comment_size == 0 ? 0U : 20U + comment_size + comment_padding;
    const std::uint32_t header = (type.tag == 3 ? 58U : 44U) + extra;
    if (samples.size() > (std::numeric_limits<std::uint32_t>::max() - header) / type.width) {
        throw std::runtime_error("audio exceeds RIFF size limit");
    }
    const auto alignment = static_cast<std::uint16_t>(type.width * options.channels);
    const auto size = static_cast<std::uint32_t>(samples.size() * type.width);
    const auto padding = size & 1U;
    binary::Bytes bytes;
    bytes.reserve(static_cast<std::size_t>(header) + size + padding);
    fourcc(bytes, "RIFF");
    write(bytes, header - 8 + size + padding);
    fourcc(bytes, "WAVE");
    fourcc(bytes, "fmt ");
    write<std::uint32_t>(bytes, type.tag == 3 ? 18 : 16);
    write(bytes, type.tag);
    write(bytes, options.channels);
    write(bytes, options.rate);
    write(bytes, options.rate * alignment);
    write(bytes, alignment);
    write(bytes, static_cast<std::uint16_t>(8 * type.width));
    if (type.tag == 3) {
        write<std::uint16_t>(bytes, 0);
        fourcc(bytes, "fact");
        write<std::uint32_t>(bytes, 4);
        write(bytes, static_cast<std::uint32_t>(samples.size() / options.channels));
    }
    if (comment_size != 0) {
        fourcc(bytes, "LIST");
        write(bytes, 12U + comment_size + comment_padding);
        fourcc(bytes, "INFO");
        fourcc(bytes, "ICMT");
        write(bytes, comment_size);
        fourcc(bytes, options.comment);
        bytes.push_back(std::byte{});
        if (comment_padding != 0) bytes.push_back(std::byte{});
    }
    fourcc(bytes, "data");
    write(bytes, size);
    const double half = std::ldexp(1.0, 8 * type.width - 1);
    auto seed = options.dither_seed;
    for (const double value : samples) {
        const double x = finite(value);
        if (options.encoding == Encoding::float32) {
            const double max = std::numeric_limits<float>::max();
            write(bytes, std::bit_cast<std::uint32_t>(static_cast<float>(std::clamp(x, -max, max))));
        } else if (options.encoding == Encoding::float64) {
            write(bytes, std::bit_cast<std::uint64_t>(x));
        } else {
            double noise = 0.0;
            if (seed != 0) {
                const double a = uniform(seed);
                noise = a - uniform(seed);
            }
            const double level = std::round(std::clamp(x, -1.0, 1.0) * half + noise);
            const auto sample = static_cast<std::int64_t>(std::clamp(level, -half, half - 1.0));
            write(bytes, static_cast<std::uint32_t>(sample), type.width);
        }
    }
    if (padding != 0) bytes.push_back(std::byte{});
    return bytes;
}

std::vector<double> map_channels(const WaveView& wave, std::uint16_t channels) {
    validate_layout(channels, wave.rate);
    const auto frames = wave.frames();
    std::vector<double> output;
    output.reserve(frames * channels);
    for (std::size_t i = 0; i < frames; ++i) {
        const auto left = wave.sample(i, 0);
        const auto right = wave.channels == 2 ? wave.sample(i, 1) : left;
        if (channels == 1) {
            output.push_back(left / 2 + right / 2);
        } else {
            output.push_back(left);
            output.push_back(right);
        }
    }
    return output;
}
}
