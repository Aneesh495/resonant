#include "resonant/vm.hpp"

#include "fp_mode.hpp"

#include "dsp.hpp"
#include "effects.hpp"
#include "resonant/spectral.hpp"
#include "numeric.hpp"

#include <bit>
#include <numbers>

namespace resonant {
namespace {
bool polyphonic(Op op) noexcept {
    return op >= Op::poly_sine && op <= Op::poly_triangle;
}

enum class Stage : std::uint8_t { idle, attack, decay, held, release };
struct Voice {
    std::uint64_t stamp{};
    std::uint32_t phase{};
    std::uint32_t step{};
    float amplitude{};
    float velocity{};
    std::uint8_t channel{};
    std::uint8_t note{};
    Stage stage{};
    bool pressed{};
};
static_assert(sizeof(Voice) <= 8 * sizeof(float));

struct Memory {
    std::vector<float> cells;
    std::vector<Voice> voices;
    std::unique_ptr<Convolver> convolution;
    dsp::Tank tank;
    std::array<std::uint32_t, 6> fir_cursors{};
    std::uint32_t cursor{};
    std::uint32_t rng{};
};

struct ParameterValue {
    float value{};
    float target{};
    float step{};
    std::uint32_t remaining{};

    void set(float target_value, std::uint32_t frames) noexcept {
        target = numeric::bounded(target_value);
        remaining = frames;
        if (frames == 0) value = target;
        step = frames == 0 ? 0.0F : (target - value) / static_cast<float>(frames);
    }

    float next() noexcept {
        if (remaining > 0) value = --remaining == 0 ? target : numeric::bounded(value + step);
        return value;
    }
};

std::uint32_t xorshift(std::uint32_t seed) noexcept {
    seed ^= seed << 13;
    seed ^= seed >> 17;
    return seed ^ (seed << 5);
}

void advance(Memory& state, float value) noexcept {
    state.cells[state.cursor] = numeric::bounded(value);
    if (++state.cursor == state.cells.size()) state.cursor = 0;
}

Voice& choose_voice(std::span<Voice> voices, const Event& event) noexcept {
    Voice* idle = nullptr;
    Voice* weakest = &voices.front();
    for (auto& voice : voices) {
        if (voice.stage != Stage::idle && voice.channel == event.channel && voice.note == event.note) return voice;
        if (voice.stage == Stage::idle && idle == nullptr) idle = &voice;
        const bool released = voice.stage == Stage::release;
        const bool weakest_released = weakest->stage == Stage::release;
        if (released != weakest_released) {
            if (released) weakest = &voice;
        } else if (voice.amplitude < weakest->amplitude ||
                   (voice.amplitude == weakest->amplitude && voice.stamp < weakest->stamp)) {
            weakest = &voice;
        }
    }
    return idle == nullptr ? *weakest : *idle;
}
}

bool valid_event(const Event& e) noexcept {
    switch (e.type) {
    case EventType::parameter:
        return e.key != 0 && std::isfinite(e.value) && std::abs(e.value) <= signal_limit && e.ramp <= 1920000;
    case EventType::note_on:
    case EventType::note_off:
        return e.channel < 16 && e.note < 128 && std::isfinite(e.value) && e.value >= 0 && e.value <= 1;
    case EventType::sustain:
        return e.channel < 16 && (e.value == 0 || e.value == 1);
    case EventType::all_off:
    case EventType::all_sound_off:
        return e.channel < 16;
    case EventType::panic:
        return true;
    }
    return false;
}

struct Vm::Impl {
    VerifiedProgram checked;
    std::uint32_t rate;
    double inverse_rate;
    Execution execution;
    const dsp::Tables& tables;
    const dsp::Halfband& halfband;
    const dsp::HqHalfband& hq_halfband;
    const dsp::HqHalfband& hq_guard;
    std::vector<float> registers;
    std::vector<float> tail;
    std::vector<Memory> memory;
    std::vector<ParameterValue> parameters;
    std::array<bool, 16> pedal{};
    std::array<std::uint32_t, 128> note_steps{};
    std::uint64_t voice_stamp{};

    Impl(const VerifiedProgram& program, std::uint32_t sample_rate, Execution mode)
        : checked(verify(program.program())), rate(sample_rate), inverse_rate(1.0 / sample_rate), execution(mode),
          tables(dsp::tables()), halfband(dsp::halfband()), hq_halfband(dsp::hq_halfband()),
          hq_guard(dsp::hq_guard()),
          registers(checked.program().registers * max_block),
          tail(checked.program().registers), memory(checked.program().states.size()),
          parameters(checked.program().parameters.size()) {
        for (std::size_t i = 0; i < memory.size(); ++i) {
            auto& slot = memory[i];
            const auto& layout = checked.program().states[i];
            if (polyphonic(layout.owner)) slot.voices.resize(voice_count);
            else if (layout.owner == Op::convolve_short || layout.owner == Op::convolve_long) {
                const bool hall = layout.owner == Op::convolve_long;
                slot.convolution = std::make_unique<Convolver>(hall ? 128 : 64, room_impulse(hall, rate));
            } else {
                slot.cells.resize(layout.words);
                if (layout.owner == Op::room) slot.tank.configure(rate);
            }
            if (layout.owner == Op::limiter) slot.cells.back() = 1;
            slot.rng = static_cast<std::uint32_t>(layout.key) | 1U;
        }
        for (std::size_t i = 0; i < parameters.size(); ++i) {
            parameters[i].set(checked.program().parameters[i].initial, 0);
        }
        for (std::size_t i = 0; i < note_steps.size(); ++i) {
            const auto hertz = 440.0 * std::exp2((static_cast<double>(i) - 69.0) / 12.0);
            note_steps[i] = dsp::phase_step(hertz, inverse_rate);
        }
    }

    bool apply(const Event& event) noexcept {
        if (!valid_event(event)) return false;
        if (event.type == EventType::parameter) {
            for (std::size_t i = 0; i < parameters.size(); ++i) {
                if (checked.program().parameters[i].key == event.key) {
                    parameters[i].set(event.value, event.ramp);
                    return true;
                }
            }
            return false;
        }
        if (event.type == EventType::sustain) pedal[event.channel] = event.value == 1;
        if (event.type == EventType::panic) pedal.fill(false);
        const auto stamp = ++voice_stamp;
        for (auto& slot : memory) {
            if (slot.voices.empty()) continue;
            if (event.type == EventType::note_on && event.value > 0) {
                auto& voice = choose_voice(slot.voices, event);
                const bool retrigger = voice.stage != Stage::idle && voice.channel == event.channel && voice.note == event.note;
                const auto phase = retrigger ? voice.phase : 0U;
                const auto envelope = retrigger ? voice.amplitude : 0.0F;
                voice = {stamp, phase, note_steps[event.note], envelope, event.value,
                         event.channel, event.note, Stage::attack, true};
                continue;
            }
            for (auto& voice : slot.voices) {
                if (event.type == EventType::panic) voice = {};
                if (voice.channel != event.channel || voice.stage == Stage::idle) continue;
                if (event.type == EventType::all_sound_off) {
                    voice = {};
                } else if (event.type == EventType::all_off) {
                    voice.pressed = false;
                    if (!pedal[event.channel]) voice.stage = Stage::release;
                } else if ((event.type == EventType::note_off || event.type == EventType::note_on) &&
                           voice.note == event.note) {
                    voice.pressed = false;
                    if (!pedal[event.channel]) voice.stage = Stage::release;
                } else if (event.type == EventType::sustain && !pedal[event.channel] && !voice.pressed) {
                    voice.stage = Stage::release;
                }
            }
        }
        return true;
    }

    float voice_sample(Op op, Memory& state, float attack, float release) const noexcept {
        const auto up = static_cast<float>(inverse_rate) / std::clamp(attack, 0.001F, 8.0F);
        const auto down = static_cast<float>(inverse_rate) / std::clamp(release, 0.002F, 12.0F);
        const auto decay = 3.5F * static_cast<float>(inverse_rate);
        float sum = 0;
        for (auto& voice : state.voices) {
            switch (voice.stage) {
            case Stage::idle: continue;
            case Stage::attack:
                voice.amplitude = std::min(1.0F, voice.amplitude + up);
                if (voice.amplitude >= 1) voice.stage = Stage::decay;
                break;
            case Stage::decay:
                voice.amplitude = std::max(0.65F, voice.amplitude - decay);
                if (voice.amplitude <= 0.65F) voice.stage = Stage::held;
                break;
            case Stage::held: break;
            case Stage::release:
                voice.amplitude -= down;
                if (voice.amplitude <= 0) {
                    voice = {};
                    continue;
                }
                break;
            }
            const auto shape = op == Op::poly_pulse ? 0.5F : 0.0F;
            sum += dsp::wave(tables, op, voice.phase, voice.step, shape) * voice.amplitude * voice.velocity;
            voice.phase += voice.step;
        }
        return sum * 0.125F;
    }

    float evaluate(const Instruction& ins, float x, float y, float z) noexcept {
        switch (ins.op) {
        case Op::literal: return ins.immediate;
        case Op::parameter: return parameters[ins.slot].next();
        case Op::audio: return x;
        case Op::add:
        case Op::sub:
        case Op::mul:
        case Op::clip:
            return numeric::arithmetic(ins.op, x, y);
        case Op::sine:
        case Op::saw:
        case Op::pulse:
        case Op::triangle: {
            auto& state = memory[ins.slot];
            const auto step = dsp::phase_step(x, inverse_rate);
            const auto output = dsp::wave(tables, ins.op, state.cursor, step, y);
            state.cursor += step;
            return output;
        }
        case Op::noise: {
            auto& state = memory[ins.slot];
            state.rng = xorshift(state.rng);
            return static_cast<float>(state.rng >> 8) * 0x1p-23F - 1.0F;
        }
        case Op::delay_read:
            return memory[ins.slot].cells[memory[ins.slot].cursor];
        case Op::lowpass:
        case Op::highpass:
        case Op::bandpass:
            return dsp::filter(ins.op, memory[ins.slot].cells.data(), x, y, z, inverse_rate);
        case Op::saturate: return dsp::saturation(x, y);
        case Op::oversaturate: {
            auto& state = memory[ins.slot];
            return dsp::oversaturate(halfband, state.cells, state.cursor, x, y);
        }
        case Op::oversaturate_hq: {
            auto& state = memory[ins.slot];
            return dsp::oversaturate_hq(hq_halfband, hq_guard, state.cells, state.fir_cursors, x, y);
        }
        case Op::limiter: {
            auto& state = memory[ins.slot];
            return dsp::limit(state.cells, state.cursor, x, y, z, inverse_rate);
        }
        case Op::align: {
            auto& state = memory[ins.slot];
            const auto previous = state.cells[state.cursor];
            advance(state, x);
            return previous;
        }
        case Op::convolve_short:
        case Op::convolve_long: {
            auto& state = memory[ins.slot];
            const auto result = state.convolution->process(x);
            const auto mix = std::clamp(y, 0.0F, 1.0F);
            return result.dry + mix * (result.wet - result.dry);
        }
        case Op::room: {
            auto& state = memory[ins.slot];
            const auto wet = state.tank.process(state.cells, x, y);
            const auto mix = std::clamp(z, 0.0F, 1.0F);
            return x + mix * (wet - x);
        }
        case Op::pan_left:
        case Op::pan_right: {
            const auto position = std::clamp(y, -1.0F, 1.0F);
            return x * std::sqrt(0.5F * (ins.op == Op::pan_left ? 1.0F - position : 1.0F + position));
        }
        case Op::poly_sine:
        case Op::poly_saw:
        case Op::poly_pulse:
        case Op::poly_triangle:
            return voice_sample(ins.op, memory[ins.slot], x, y);
        case Op::follower: {
            auto& level = memory[ins.slot].cells[0];
            const auto target = std::abs(x);
            level = numeric::bounded(dsp::envelope(level, target, target > level ? y : z, inverse_rate));
            return level;
        }
        case Op::compressor:
        case Op::gate: {
            auto& gain = memory[ins.slot].cells[0];
            const auto threshold = std::clamp(y, 0.0001F, 1.0F);
            const auto magnitude = std::abs(x);
            const auto target = ins.op == Op::gate ? (magnitude >= threshold ? 1.0F : 0.0F) :
                (magnitude <= threshold ? 1.0F : (threshold + 0.25F * (magnitude - threshold)) / magnitude);
            gain = dsp::envelope(gain, target, target < gain ? 0.003F : z, inverse_rate);
            return gain * x;
        }
        case Op::delay_mod:
        case Op::allpass: {
            auto& state = memory[ins.slot];
            const auto delayed = dsp::fractional_delay(state.cells, state.cursor, y);
            const auto feedback = std::clamp(z, -0.97F, 0.97F);
            const auto out = ins.op == Op::allpass ? delayed - feedback * x : delayed;
            advance(state, x + feedback * (ins.op == Op::allpass ? out : delayed));
            return out;
        }
        default: return 0;
        }
    }

    void execute(float* out, std::size_t frames, RenderStats& stats) noexcept {
        const auto reg = [&](std::uint16_t i) { return registers.data() + i * max_block; };
        for (const auto& ins : checked.program().instructions) {
            const float* x = reg(ins.in[0]);
            const float* y = reg(ins.in[1]);
            const float* z = reg(ins.in[2]);
            if (ins.op == Op::output) {
                for (std::size_t i = 0; i < frames; ++i) {
                    out[2 * i] = std::clamp(x[i], -1.0F, 1.0F);
                    out[2 * i + 1] = std::clamp(y[i], -1.0F, 1.0F);
                    stats.clipped += static_cast<std::uint32_t>(std::abs(x[i]) > 1) +
                                     static_cast<std::uint32_t>(std::abs(y[i]) > 1);
                    stats.peak = std::max({stats.peak, std::abs(out[2 * i]), std::abs(out[2 * i + 1])});
                }
            } else if (ins.op == Op::delay_write) {
                advance(memory[ins.slot], x[0]);
            } else {
                auto* dst = reg(ins.dst);
                if (frames >= 4 && dsp::vector_arithmetic(ins.op, dst, x, y, frames)) continue;
                for (std::size_t i = 0; i < frames; ++i) dst[i] = numeric::bounded(evaluate(ins, x[i], y[i], z[i]));
            }
        }
    }
};

Vm::Vm(const VerifiedProgram& program, std::uint32_t rate, Execution execution) {
    if (rate < 8000 || rate > 192000) throw std::runtime_error("sample rate outside 8000 to 192000 Hz");
    impl_ = std::make_unique<Impl>(program, rate, execution);
}

Vm::~Vm() = default;

const VerifiedProgram& Vm::verified() const noexcept { return impl_->checked; }

std::uint32_t Vm::sample_rate() const noexcept { return impl_->rate; }

std::span<const float> Vm::tail_registers() const noexcept { return impl_->tail; }

bool Vm::event(const Event& event) noexcept { return impl_->apply(event); }

bool float_mode_available() noexcept { return numeric::native_float_mode; }

RenderStats Vm::render(std::span<float> interleaved) noexcept {
    [[maybe_unused]] const numeric::FloatMode mode;
    RenderStats stats{};
    const auto frames = interleaved.size() / 2;
    if (frames == 0 || frames > max_block || interleaved.size() % 2 != 0) return stats;
    const bool samplewise = impl_->execution == Execution::scalar || impl_->checked.has_feedback();
    if (samplewise) {
        for (std::size_t i = 0; i < frames; ++i) impl_->execute(interleaved.data() + 2 * i, 1, stats);
    } else {
        impl_->execute(interleaved.data(), frames, stats);
    }
    stats.instructions = frames * impl_->checked.program().instructions.size();
    for (std::size_t i = 0; i < impl_->tail.size(); ++i) {
        impl_->tail[i] = impl_->registers[i * max_block + (samplewise ? 0 : frames - 1)];
    }
    for (const auto& state : impl_->memory) {
        for (const auto& voice : state.voices) stats.voices += static_cast<std::uint32_t>(voice.stage != Stage::idle);
    }
    return stats;
}

Vm::Migration Vm::migration_from(const Vm& previous) const noexcept {
    constexpr std::uint32_t max_copy_words = 2048;
    Migration plan;
    plan.states.fill(-1);
    plan.parameters.fill(-1);
    plan.compatible_rate = sample_rate() == previous.sample_rate();
    if (!plan.compatible_rate) return plan;
    const auto& next = impl_->checked.program();
    const auto& old = previous.impl_->checked.program();
    std::uint32_t copied = 0;
    for (std::size_t i = 0; i < next.states.size(); ++i) {
        const auto& slot = next.states[i];
        if (slot.words > max_copy_words - copied) continue;
        for (std::size_t j = 0; j < old.states.size(); ++j) {
            const auto& candidate = old.states[j];
            if (candidate.key == slot.key && candidate.owner == slot.owner && candidate.words == slot.words) {
                plan.states[i] = static_cast<std::int16_t>(j);
                copied += slot.words;
                break;
            }
        }
    }
    for (std::size_t i = 0; i < next.parameters.size(); ++i) {
        for (std::size_t j = 0; j < old.parameters.size(); ++j) {
            if (next.parameters[i].key == old.parameters[j].key) {
                plan.parameters[i] = static_cast<std::int16_t>(j);
                break;
            }
        }
    }
    return plan;
}

void Vm::migrate(const Vm& previous, const Migration& plan) noexcept {
    if (!plan.compatible_rate) return;
    impl_->pedal = previous.impl_->pedal;
    impl_->voice_stamp = previous.impl_->voice_stamp;
    for (std::size_t i = 0; i < impl_->memory.size(); ++i) {
        if (plan.states[i] < 0) continue;
        auto& to = impl_->memory[i];
        const auto& from = previous.impl_->memory[static_cast<std::size_t>(plan.states[i])];
        std::ranges::copy(from.cells, to.cells.begin());
        std::ranges::copy(from.voices, to.voices.begin());
        to.cursor = from.cursor;
        to.rng = from.rng;
        to.fir_cursors = from.fir_cursors;
    }
    for (std::size_t i = 0; i < impl_->parameters.size(); ++i) {
        if (plan.parameters[i] >= 0) {
            impl_->parameters[i] = previous.impl_->parameters[static_cast<std::size_t>(plan.parameters[i])];
        }
    }
}
}
