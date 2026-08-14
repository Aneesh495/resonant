#pragma once

#include "resonant/bytecode.hpp"

#include <memory>

namespace resonant {
enum class Execution { scalar, block };

enum class EventType : std::uint8_t { parameter, note_on, note_off, sustain, all_off, panic, all_sound_off };

struct Event {
    std::uint64_t frame{};
    EventType type{};
    std::uint8_t channel{};
    std::uint8_t note{};
    std::uint64_t key{};
    float value{};
    std::uint32_t ramp{};
};

[[nodiscard]] bool valid_event(const Event& event) noexcept;
[[nodiscard]] bool simd_available() noexcept;
[[nodiscard]] bool float_mode_available() noexcept;

struct RenderStats {
    std::uint64_t instructions{};
    std::uint32_t clipped{};
    std::uint32_t voices{};
    float peak{};
};

class Engine;

class Vm {
public:
    Vm(const VerifiedProgram& program, std::uint32_t sample_rate, Execution execution = Execution::block);
    ~Vm();
    Vm(const Vm&) = delete;
    Vm& operator=(const Vm&) = delete;

    [[nodiscard]] const VerifiedProgram& verified() const noexcept;
    [[nodiscard]] std::uint32_t sample_rate() const noexcept;
    [[nodiscard]] std::span<const float> tail_registers() const noexcept;
    [[nodiscard]] RenderStats render(std::span<float> interleaved) noexcept;
    [[nodiscard]] bool event(const Event& event) noexcept;

private:
    friend class Engine;
    struct Impl;
    std::unique_ptr<Impl> impl_;

    struct Migration {
        std::array<std::int16_t, max_states> states;
        std::array<std::int16_t, max_parameters> parameters;
        bool compatible_rate = false;
    };
    [[nodiscard]] Migration migration_from(const Vm& previous) const noexcept;
    void migrate(const Vm& previous, const Migration& plan) noexcept;
};
}
