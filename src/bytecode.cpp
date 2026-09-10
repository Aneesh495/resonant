#include "resonant/bytecode.hpp"

#include "bytes.hpp"
#include "effects.hpp"
#include "dsp.hpp"

#include <algorithm>

#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace resonant {
namespace {
constexpr std::array<Opcode, static_cast<std::size_t>(Op::count)> contracts{{
    {"literal",      0, Rule::control, true,  1,  0},
    {"parameter",    0, Rule::control, true,  4,  0},
    {"add",          2, Rule::join,    true,  4,  0},
    {"sub",          2, Rule::join,    true,  4,  0},
    {"mul",          2, Rule::join,    true,  4,  0},
    {"clip",         2, Rule::join,    true,  8,  0},
    {"audio",        1, Rule::audio,   true,  1,  0},
    {"sine",         2, Rule::audio,   true,  28, 1},
    {"saw",          1, Rule::audio,   true,  32, 1},
    {"pulse",        2, Rule::audio,   true,  52, 1},
    {"triangle",     1, Rule::audio,   true,  80, 1},
    {"noise",        0, Rule::audio,   true,  12, 1},
    {"delay.read",   0, Rule::audio,   true,  3,  1},
    {"delay.write",  1, Rule::first_audio, false, 5, 0},
    {"lowpass",      3, Rule::first_audio, true,  45, 2},
    {"highpass",     3, Rule::first_audio, true,  45, 2},
    {"bandpass",     3, Rule::first_audio, true,  45, 2},
    {"saturate",     2, Rule::first_audio, true,  16, 0},
    {"pan_left",     2, Rule::first_audio, true,  16, 0},
    {"pan_right",    2, Rule::first_audio, true,  16, 0},
    {"poly_sine",    2, Rule::audio,   true,  64 * voice_count, voice_words},
    {"poly_saw",     2, Rule::audio,   true,  72 * voice_count, voice_words},
    {"poly_pulse",   2, Rule::audio,   true,  88 * voice_count, voice_words},
    {"poly_triangle",2, Rule::audio,   true,  112 * voice_count, voice_words},
    {"follower",     3, Rule::first_audio, true,  22, 1},
    {"compressor",   3, Rule::first_audio, true,  28, 1},
    {"gate",         3, Rule::first_audio, true,  28, 1},
    {"delay_mod",    3, Rule::first_audio, true,  34, 65536},
    {"allpass",      3, Rule::first_audio, true,  25, 8192},
    {"out",          2, Rule::first_audio, false, 4,  0},
    {"align",        1, Rule::first_audio, true, 8, 1},
    {"convolve_short", 2, Rule::first_audio, true, 18500, 8192, 64},
    {"convolve_long",  2, Rule::first_audio, true, 42000, 16384, 128},
    {"room",         3, Rule::first_audio, true, 160, dsp::tank_words},
    {"oversaturate", 2, Rule::first_audio, true, 1350, 2 * dsp::halfband_length, dsp::oversampling_latency},
    {"limiter",      3, Rule::first_audio, true, 170, dsp::limiter_latency + 1, dsp::limiter_latency},
    {"oversaturate_hq", 2, Rule::first_audio, true, 12000, dsp::hq_state_words, dsp::hq_latency},
    {"convolve_ir",  2, Rule::first_audio, true, 42000, 16384, 128},
    {"wavetable",    2, Rule::audio, true, 46, dsp::wavetable_words}
}};

constexpr std::uint32_t magic = 0x43534252; // RBSC
constexpr std::size_t header_bytes = 20;
constexpr std::size_t record_bytes = 16;
static_assert(std::numeric_limits<float>::is_iec559 && sizeof(float) == 4);

bool bounded(float x) {
    return std::isfinite(x) && std::abs(x) <= signal_limit;
}

void require(bool valid, const char* message) {
    if (!valid) throw std::runtime_error(message);
}

bool canonical_zero(float value) {
    return std::bit_cast<std::uint32_t>(value) == 0;
}

void validate_counts(std::size_t registers, std::size_t states, std::size_t parameters, std::size_t code) {
    require(registers > 0 && registers <= max_registers, "invalid register count");
    require(states <= max_states, "invalid state count");
    require(parameters <= max_parameters, "invalid parameter count");
    require(code > 0 && code <= max_instructions, "invalid instruction count");
}
}

const Opcode& opcode(Op op) {
    const auto index = static_cast<std::size_t>(op);
    require(index < contracts.size(), "unknown opcode");
    return contracts[index];
}

std::uint64_t name_key(std::string_view name) noexcept {
    std::uint64_t result = 14695981039346656037ULL;
    for (const char c : name) {
        result ^= static_cast<unsigned char>(c);
        result *= 1099511628211ULL;
    }
    return result;
}

VerifiedProgram::VerifiedProgram(Program program, std::uint32_t work, std::uint32_t words,
                                 bool delay, std::array<std::uint32_t, 2> latency)
    : program_(std::move(program)), work_(work), words_(words), delay_(delay), latency_(latency) {}

bool valid_wavetable(std::span<const float> samples) noexcept {
    return samples.size() >= 16 && samples.size() <= max_asset_samples && std::has_single_bit(samples.size()) &&
           std::ranges::all_of(samples, [](float x) { return std::isfinite(x) && std::abs(x) <= 1; });
}

bool valid_impulse(std::span<const float> samples, std::uint32_t rate) noexcept {
    if (samples.empty() || samples.size() > max_asset_samples || rate < 8000 || rate > 192000) return false;
    double gain = 0;
    for (float tap : samples) {
        if (!std::isfinite(tap) || std::abs(tap) > 1) return false;
        gain += std::abs(tap);
    }
    return gain <= 8;
}

VerifiedProgram verify(Program program) {
    validate_counts(program.registers, program.states.size(), program.parameters.size(), program.instructions.size());
    std::uint32_t words = 0;
    for (std::size_t i = 0; i < program.states.size(); ++i) {
        const auto& state = program.states[i];
        const auto& contract = opcode(state.owner);
        require(state.key != 0, "zero state identity");
        if (state.owner == Op::convolve_ir) {
            require(valid_impulse(state.asset, state.asset_rate), "impulse asset exceeds its rate, extent or gain bound");
        } else if (state.owner == Op::wavetable) {
            require(state.asset_rate == 0 && valid_wavetable(state.asset), "invalid wavetable attachment");
        } else {
            require(state.asset_rate == 0 && state.asset.empty(), "asset attached to an incompatible state");
        }
        require(contract.state_words != 0, "stateless opcode owns state");
        if (state.owner == Op::delay_read || state.owner == Op::align) {
            require(state.words >= 1 && state.words <= max_delay, "delay extent out of bounds");
        } else {
            require(state.words == contract.state_words, "invalid fixed state extent");
        }
        for (std::size_t j = 0; j < i; ++j) require(program.states[j].key != state.key, "duplicate state identity");
        require(state.words <= max_state_words - words, "state budget exceeded");
        words += state.words;
    }
    for (std::size_t i = 0; i < program.parameters.size(); ++i) {
        const auto& parameter = program.parameters[i];
        require(parameter.key != 0 && bounded(parameter.initial), "invalid parameter");
        for (std::size_t j = 0; j < i; ++j) {
            require(program.parameters[j].key != parameter.key, "duplicate parameter identity");
        }
    }

    std::array<bool, max_registers> defined{};
    std::array<Rate, max_registers> rates{};
    std::array<std::uint32_t, max_registers> latency{};
    std::array<bool, max_states> state_seen{};
    std::array<bool, max_states> writes{};
    std::array<bool, max_parameters> parameter_seen{};
    std::uint32_t work = 0;
    bool epilogue = false;
    bool delay = false;
    for (std::size_t i = 0; i < program.instructions.size(); ++i) {
        const auto& ins = program.instructions[i];
        const auto& contract = opcode(ins.op);
        const bool last = i + 1 == program.instructions.size();
        require((ins.op == Op::output) == last, "one terminal output is required");
        if (epilogue) require(ins.op == Op::delay_write || last, "instruction after delay commit");
        require(contract.work <= max_work_per_frame - work, "instruction work budget exceeded");
        work += contract.work;

        bool has_audio = false;
        std::uint32_t input_latency = 0;
        for (std::size_t j = 0; j < ins.in.size(); ++j) {
            const auto reg = ins.in[j];
            if (j >= contract.inputs) {
                require(reg == 0, "nonzero unused input operand");
                continue;
            }
            require(reg < program.registers && defined[reg], "undefined or out-of-bounds source register");
            require(!contract.writes_register || ins.dst != reg, "destination aliases source");
            has_audio |= rates[reg] == Rate::audio;
            input_latency = std::max(input_latency, latency[reg]);
        }
        if (contract.rule == Rule::first_audio) {
            require(rates[ins.in[0]] == Rate::audio, "first input must be audio rate");
        }
        if (ins.op == Op::output) require(rates[ins.in[1]] == Rate::audio, "right output must be audio rate");
        const auto expected_rate =
            contract.rule == Rule::audio || contract.rule == Rule::first_audio ||
            (contract.rule == Rule::join && has_audio) ? Rate::audio : Rate::control;
        require(ins.rate == expected_rate, "invalid instruction rate");
        if (contract.writes_register) {
            require(ins.dst < program.registers, "destination register out of bounds");
            defined[ins.dst] = true;
            rates[ins.dst] = ins.rate;
            latency[ins.dst] = input_latency + contract.latency;
        } else {
            require(ins.dst == 0, "nonzero unused destination operand");
        }

        if (contract.state_words != 0) {
            require(ins.slot < program.states.size(), "state slot out of bounds");
            require(program.states[ins.slot].owner == ins.op && !state_seen[ins.slot], "invalid or repeated state owner");
            state_seen[ins.slot] = true;
            if (ins.op == Op::align) latency[ins.dst] += program.states[ins.slot].words;
        } else if (ins.op == Op::delay_write) {
            epilogue = true;
            delay = true;
            require(ins.slot < program.states.size(), "delay write slot out of bounds");
            require(program.states[ins.slot].owner == Op::delay_read && state_seen[ins.slot] &&
                    !writes[ins.slot], "unpaired or repeated delay write");
            writes[ins.slot] = true;
        } else if (ins.op == Op::parameter) {
            require(ins.slot < program.parameters.size() && !parameter_seen[ins.slot], "invalid or repeated parameter slot");
            parameter_seen[ins.slot] = true;
        } else {
            require(ins.slot == 0, "nonzero unused slot operand");
        }

        require(ins.op == Op::literal ? bounded(ins.immediate) : canonical_zero(ins.immediate),
                "invalid or noncanonical immediate");
    }

    for (std::size_t i = 0; i < program.states.size(); ++i) {
        require(state_seen[i] && (program.states[i].owner != Op::delay_read || writes[i]), "orphan state slot");
    }
    for (std::size_t i = 0; i < program.parameters.size(); ++i) {
        require(parameter_seen[i], "orphan parameter slot");
    }
    const auto inputs = program.instructions.back().in;
    return VerifiedProgram(std::move(program), work, words, delay, {latency[inputs[0]], latency[inputs[1]]});
}

std::uint16_t format_version(const VerifiedProgram& checked) noexcept {
    return std::ranges::any_of(checked.program().states, [](const auto& s) { return !s.asset.empty(); })
        ? bytecode_version : bytecode_version_without_assets;
}

std::vector<std::byte> encode(const VerifiedProgram& checked) {
    using binary::write;
    const auto& program = checked.program();
    binary::Bytes bytes;
    bytes.reserve(header_bytes + record_bytes * (program.states.size() + program.parameters.size() + program.instructions.size()));
    write(bytes, magic);
    write(bytes, format_version(checked));
    write(bytes, program.registers);
    write(bytes, static_cast<std::uint16_t>(program.states.size()));
    write(bytes, static_cast<std::uint16_t>(program.parameters.size()));
    write(bytes, static_cast<std::uint32_t>(program.instructions.size()));
    write<std::uint32_t>(bytes, 0);
    for (const auto& state : program.states) {
        write(bytes, state.key);
        write(bytes, static_cast<std::uint32_t>(state.owner));
        write(bytes, state.words);
    }
    for (const auto& parameter : program.parameters) {
        write(bytes, parameter.key);
        write(bytes, std::bit_cast<std::uint32_t>(parameter.initial));
        write<std::uint32_t>(bytes, 0);
    }
    for (const auto& ins : program.instructions) {
        write(bytes, static_cast<std::uint8_t>(ins.op));
        write(bytes, static_cast<std::uint8_t>(ins.rate));
        write(bytes, ins.dst);
        for (const auto reg : ins.in) write(bytes, reg);
        write(bytes, ins.slot);
        write(bytes, std::bit_cast<std::uint32_t>(ins.immediate));
    }
    for (const auto& state : program.states) {
        if (state.asset.empty()) continue;
        write(bytes, state.asset_rate);
        write(bytes, static_cast<std::uint32_t>(state.asset.size()));
        for (const float sample : state.asset) write(bytes, std::bit_cast<std::uint32_t>(sample));
    }
    return bytes;
}

std::uint64_t program_key(const VerifiedProgram& program) {
    const auto bytes = encode(program);
    return name_key({reinterpret_cast<const char*>(bytes.data()), bytes.size()});
}

VerifiedProgram decode(std::span<const std::byte> bytes) {
    using binary::read;
    require(read<std::uint32_t>(bytes, 0) == magic, "invalid bytecode magic");
    const auto version = read<std::uint16_t>(bytes, 4);
    const bool assets = version == bytecode_version;
    require(assets || version == bytecode_version_without_assets, "unsupported bytecode version");
    Program program{};
    program.registers = read<std::uint16_t>(bytes, 6);
    const auto states = read<std::uint16_t>(bytes, 8);
    const auto parameters = read<std::uint16_t>(bytes, 10);
    const auto instructions = read<std::uint32_t>(bytes, 12);
    validate_counts(program.registers, states, parameters, instructions);
    require(read<std::uint32_t>(bytes, 16) == 0, "nonzero reserved header");
    const auto records = static_cast<std::size_t>(states) + parameters + instructions;
    const auto prefix = header_bytes + records * record_bytes;
    require(bytes.size() >= prefix && bytes.size() <= max_bytecode_bytes && (assets || bytes.size() == prefix),
            "incorrect bytecode length");
    std::size_t pos = header_bytes;
    program.states.reserve(states);
    program.parameters.reserve(parameters);
    program.instructions.reserve(instructions);
    for (std::size_t i = 0; i < states; ++i, pos += record_bytes) {
        const auto owner = read<std::uint32_t>(bytes, pos + 8);
        require(owner < static_cast<std::uint32_t>(Op::count), "invalid state owner opcode");
        program.states.push_back({read<std::uint64_t>(bytes, pos), static_cast<Op>(owner),
                                  read<std::uint32_t>(bytes, pos + 12)});
    }
    for (std::size_t i = 0; i < parameters; ++i, pos += record_bytes) {
        require(read<std::uint32_t>(bytes, pos + 12) == 0, "nonzero reserved parameter word");
        program.parameters.push_back({read<std::uint64_t>(bytes, pos),
                                      std::bit_cast<float>(read<std::uint32_t>(bytes, pos + 8))});
    }
    for (std::size_t i = 0; i < instructions; ++i, pos += record_bytes) {
        Instruction ins{};
        ins.op = static_cast<Op>(read<std::uint8_t>(bytes, pos));
        ins.rate = static_cast<Rate>(read<std::uint8_t>(bytes, pos + 1));
        ins.dst = read<std::uint16_t>(bytes, pos + 2);
        for (std::size_t j = 0; j < ins.in.size(); ++j) {
            ins.in[j] = read<std::uint16_t>(bytes, pos + 4 + 2 * j);
        }
        ins.slot = read<std::uint16_t>(bytes, pos + 10);
        ins.immediate = std::bit_cast<float>(read<std::uint32_t>(bytes, pos + 12));
        program.instructions.push_back(ins);
    }
    if (assets) {
        bool found = false;
        for (auto& state : program.states) {
            if (state.owner != Op::convolve_ir && state.owner != Op::wavetable) continue;
            found = true;
            state.asset_rate = read<std::uint32_t>(bytes, pos);
            const auto count = read<std::uint32_t>(bytes, pos + 4);
            pos += 8;
            require(count > 0 && count <= max_asset_samples && count <= (bytes.size() - pos) / 4,
                    "invalid sample attachment extent");
            state.asset.reserve(count);
            for (std::uint32_t i = 0; i < count; ++i, pos += 4) {
                state.asset.push_back(std::bit_cast<float>(read<std::uint32_t>(bytes, pos)));
            }
        }
        require(found && pos == bytes.size(), "missing sample attachments or trailing data");
    }
    return verify(std::move(program));
}
}
