#pragma once

#include "resonant/midi.hpp"

#include <string>

namespace resonant::apple {
struct MidiSource {
    std::uint32_t endpoint;
    std::string name;
};

class MidiInput {
public:
    MidiInput();
    ~MidiInput();
    MidiInput(const MidiInput&) = delete;
    MidiInput& operator=(const MidiInput&) = delete;

    [[nodiscard]] static std::vector<MidiSource> sources();
    void connect(std::uint32_t endpoint);
    void disconnect() noexcept;
    [[nodiscard]] bool poll(Event& event) noexcept;
    [[nodiscard]] std::uint64_t dropped() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

[[nodiscard]] double host_seconds_per_tick() noexcept;
}
