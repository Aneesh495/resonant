#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace resonant {
inline constexpr std::uint16_t bytecode_version = 1;
inline constexpr std::size_t max_registers = 256;
inline constexpr std::size_t max_instructions = 1024;
inline constexpr std::size_t max_states = 128;
inline constexpr std::size_t max_parameters = 128;
inline constexpr std::size_t max_block = 512;
inline constexpr std::uint32_t max_state_words = 1048576;
inline constexpr std::uint32_t max_work_per_frame = 131072;
inline constexpr std::uint32_t max_delay = 131072;
inline constexpr std::uint32_t voice_count = 64;
inline constexpr float signal_limit = 1048576.0F;
inline constexpr std::uint32_t voice_words = 8 * voice_count;

enum class Rate : std::uint8_t { control, audio };
enum class Op : std::uint8_t {
    literal, parameter, add, sub, mul, clip, audio,
    sine, saw, pulse, triangle, noise, delay_read, delay_write,
    lowpass, highpass, bandpass, saturate, pan_left, pan_right,
    poly_sine, poly_saw, poly_pulse, poly_triangle,
    follower, compressor, gate, delay_mod, allpass, output, count
};

struct Instruction {
    Op op{};
    Rate rate{};
    std::uint16_t dst{};
    std::array<std::uint16_t, 3> in{};
    std::uint16_t slot{};
    float immediate{};
};

struct StateLayout {
    std::uint64_t key{};
    Op owner{};
    std::uint32_t words{};
};

struct Parameter {
    std::uint64_t key{};
    float initial{};
};

struct Program {
    std::uint16_t registers{};
    std::vector<StateLayout> states;
    std::vector<Parameter> parameters;
    std::vector<Instruction> instructions;
};

enum class Rule : std::uint8_t { control, join, audio, first_audio };
struct Opcode {
    std::string_view name;
    std::uint8_t inputs;
    Rule rule;
    bool writes_register;
    std::uint32_t work;
    // A delay ring is variable size. Other owners have fixed storage.
    std::uint32_t state_words;
};

[[nodiscard]] const Opcode& opcode(Op op);
[[nodiscard]] std::uint64_t name_key(std::string_view name) noexcept;

class VerifiedProgram {
public:
    [[nodiscard]] const Program& program() const noexcept { return program_; }
    [[nodiscard]] std::uint32_t work_per_frame() const noexcept { return work_; }
    [[nodiscard]] std::uint32_t state_words() const noexcept { return words_; }
    [[nodiscard]] bool has_feedback() const noexcept { return delay_; }

private:
    friend VerifiedProgram verify(Program);
    VerifiedProgram(Program, std::uint32_t, std::uint32_t, bool);
    Program program_;
    std::uint32_t work_;
    std::uint32_t words_;
    bool delay_;
};

[[nodiscard]] VerifiedProgram verify(Program program);
[[nodiscard]] std::vector<std::byte> encode(const VerifiedProgram& program);
[[nodiscard]] VerifiedProgram decode(std::span<const std::byte> bytes);
}
