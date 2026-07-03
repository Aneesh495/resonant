#include "resonant/wave.hpp"
#include "test.hpp"

#include <array>
#include <cmath>
#include <limits>
#include <random>

using namespace resonant;

int main() {
    // Independent hand-packed mono fixture: odd unknown chunk, positive and negative extrema.
    constexpr unsigned char fixture[] = {
        82,73,70,70,52,0,0,0,87,65,86,69,
        74,85,78,75,1,0,0,0,42,0,
        102,109,116,32,16,0,0,0,1,0,1,0,128,187,0,0,128,50,2,0,3,0,24,0,
        100,97,116,97,6,0,0,0,0,0,128,255,255,127
    };
    const auto bytes = std::as_bytes(std::span{fixture});
    const auto wave = parse_wave(bytes);
    CHECK(wave.rate == 48000 && wave.frames() == 2 && wave.channels == 1);
    CHECK(wave.sample(0, 0) == -1 && wave.sample(1, 0) == 1 - 0x1p-23);
    CHECK(map_channels(wave, 2) == std::vector<double>({-1,-1,1-0x1p-23,1-0x1p-23}));
    for (std::size_t size = 0; size < bytes.size(); ++size) {
        rejects([&] { (void)parse_wave(bytes.first(size)); });
    }
    std::vector<std::byte> broken(bytes.begin(), bytes.end());
    broken[42] = std::byte{2};
    rejects([&] { (void)parse_wave(broken); });
    broken.assign(bytes.begin(), bytes.end());
    broken[0] = std::byte{'X'};
    rejects([&] { (void)parse_wave(broken); });

    std::mt19937 random(0x12def);
    for (const auto type : {Encoding::pcm16, Encoding::pcm24, Encoding::pcm32,
                            Encoding::float32, Encoding::float64}) {
        const bool floating = type >= Encoding::float32;
        const int bits = type == Encoding::pcm16 ? 16 : type == Encoding::pcm24 ? 24 : 32;
        const auto scale = std::int64_t{1} << (bits - 1);
        for (int trial = 0; trial < 25000; ++trial) {
            const auto signed_value = static_cast<std::int64_t>(random() % (2ULL * static_cast<std::uint64_t>(scale))) - scale;
            const double sample = static_cast<double>(signed_value) / static_cast<double>(scale);
            auto encoded = encode_wave(std::span(&sample, 1), {type, 44100, 1, 0});
            const auto decoded = parse_wave(encoded);
            CHECK(decoded.frames() == 1 && decoded.encoding == type);
            CHECK(decoded.sample(0, 0) == (type == Encoding::float32 ? static_cast<float>(sample) : sample));
            if (!floating) {
                const auto word = static_cast<std::uint64_t>(signed_value);
                for (int i = 0; i < bits / 8; ++i) {
                    CHECK(decoded.payload[static_cast<std::size_t>(i)] == static_cast<std::byte>(word >> (i * 8)));
                }
            }
            encoded[4] ^= std::byte{1};
            rejects([&] { (void)parse_wave(encoded); });
        }
    }
    const std::array extremes{-2.0, 2.0, std::numeric_limits<double>::quiet_NaN(), 0x1p-16, -0x1p-16};
    const auto clipped = encode_wave(extremes, {Encoding::pcm16, 48000, 1, 0});
    CHECK(map_channels(parse_wave(clipped), 1) == std::vector<double>({-1,1-0x1p-15,0,0x1p-15,-0x1p-15}));
    const auto tagged = encode_wave(extremes, {Encoding::pcm16, 48000, 1, 0, "take A"});
    CHECK(parse_wave(tagged).frames() == extremes.size());
    const std::string_view tag(reinterpret_cast<const char*>(tagged.data()), tagged.size());
    CHECK(tag.find("LIST") != tag.npos && tag.find("INFOICMT") != tag.npos && tag.find("take A") != tag.npos);

    const std::vector<double> silence(100000, 0.0);
    const auto dithered = encode_wave(silence, {Encoding::pcm16, 48000, 1, 991});
    const auto dither = parse_wave(dithered);
    std::array<std::size_t, 3> histogram{};
    for (std::size_t i = 0; i < silence.size(); ++i) {
        const double x = dither.sample(i, 0) * 32768.0;
        CHECK(x >= -1 && x <= 1);
        ++histogram[static_cast<std::size_t>(x + 1)];
    }
    CHECK(histogram[0] > 12000 && histogram[0] < 13000);
    CHECK(histogram[2] > 12000 && histogram[2] < 13000);
    std::cout << "125000 round trips, 125000 corrupt lengths, 100000 dither samples\n";
}
