#pragma once

#include "resonant/bytecode.hpp"

#include <stdexcept>
#include <string>

namespace resonant {
inline constexpr std::size_t max_source_bytes = 128 * 1024;
inline constexpr std::size_t max_declarations = 512;

struct SourceSpan {
    std::uint32_t offset{};
    std::uint32_t length{};
    std::uint32_t line{};
    std::uint32_t column{};
};

class Diagnostic : public std::runtime_error {
public:
    Diagnostic(SourceSpan span, std::string message);
    [[nodiscard]] SourceSpan span() const noexcept { return span_; }

private:
    SourceSpan span_;
};

struct Argument {
    SourceSpan span;
    std::string reference;
    float value{};
};

enum class Declaration : std::uint8_t { literal, parameter, node };

struct Expression {
    Declaration type;
    std::string name;
    std::string operation;
    SourceSpan span;
    std::vector<Argument> arguments;
};

struct SignalAsset {
    SourceSpan span;
    std::string name;
    std::uint32_t rate; // Zero denotes a periodic wavetable.
    std::vector<float> samples;
};

struct Graph {
    std::vector<SignalAsset> assets;
    std::vector<Expression> expressions;
    std::vector<Argument> outputs;
    SourceSpan output_span;
};

[[nodiscard]] Graph parse_graph(std::string_view source);
struct WaveView;
[[nodiscard]] std::string impulse_declaration(std::string_view name, const WaveView& wave);
[[nodiscard]] std::string wavetable_declaration(std::string_view name, const WaveView& wave);

struct ScheduleEntry {
    SourceSpan span;
    std::string name;
    std::vector<std::string> dependencies;
    std::size_t last_use;
};

struct Compilation {
    VerifiedProgram program;
    std::vector<ScheduleEntry> schedule;
    std::size_t eliminated;
    std::size_t folded;
};

[[nodiscard]] Compilation compile(std::string_view source);
}
