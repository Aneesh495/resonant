#pragma once

#include "resonant/engine.hpp"

#include <filesystem>
#include <string>

namespace resonant {
inline constexpr std::uint32_t ticks_per_quarter = 960;
inline constexpr std::uint64_t max_tick = 1000000000;
inline constexpr std::size_t max_session_bytes = 2 * 1024 * 1024;
inline constexpr std::uint64_t max_render_frames = 10000000;

struct Tempo {
    std::uint64_t tick;
    std::uint32_t microseconds_per_quarter;
};

class TempoMap {
public:
    explicit TempoMap(std::span<const Tempo> changes);
    [[nodiscard]] std::uint64_t frame(std::uint64_t tick, std::uint32_t rate) const;
    [[nodiscard]] std::uint64_t tick(std::uint64_t frame, std::uint32_t rate) const;

private:
    struct Segment {
        Tempo change;
        std::uint64_t elapsed;
    };
    std::vector<Segment> segments_;
};

struct Note {
    std::uint64_t tick;
    std::uint64_t duration;
    std::uint8_t channel;
    std::uint8_t pitch;
    float velocity;
};

struct Automation {
    std::uint64_t tick;
    std::string parameter;
    float value;
    std::uint32_t ramp_frames;
};

struct Pedal {
    std::uint64_t tick;
    std::uint8_t channel;
    bool down;
};

struct Loop {
    std::uint64_t begin{};
    std::uint64_t end{};
    std::uint32_t passes{1};
};

struct Session {
    std::uint64_t length = 4 * ticks_per_quarter;
    std::vector<Tempo> tempo{{0, 500000}};
    std::vector<Note> notes;
    std::vector<Automation> automation;
    std::vector<Pedal> pedal;
    Loop loop;
    std::string source;
};

struct Timeline {
    std::uint64_t frames;
    std::vector<Event> events;
};

[[nodiscard]] Session parse_session(std::string_view contents);
[[nodiscard]] std::string encode_session(const Session& session);
[[nodiscard]] Timeline schedule(const Session& session, std::uint32_t sample_rate);

[[nodiscard]] std::vector<std::byte> read_file(const std::filesystem::path& path, std::size_t limit);
[[nodiscard]] std::string read_text(const std::filesystem::path& path, std::size_t limit);
void atomic_write(const std::filesystem::path& path, std::span<const std::byte> bytes);
void atomic_write(const std::filesystem::path& path, std::string_view text);

struct OfflineRender {
    std::vector<float> samples;
    std::uint64_t instructions{};
    std::uint64_t clipped{};
    float peak{};
};

[[nodiscard]] OfflineRender render_offline(const VerifiedProgram& program, const Timeline& timeline,
                                           std::uint32_t sample_rate, std::size_t block_size,
                                           Execution execution = Execution::block);
}
