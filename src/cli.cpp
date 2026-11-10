#include "resonant/device.hpp"
#include "resonant/graph.hpp"
#include "resonant/session.hpp"
#include "resonant/wave.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <csignal>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <thread>

namespace {
using namespace resonant;
constexpr std::size_t max_bytecode_bytes = 20 + 16 * (max_instructions + max_states + max_parameters);
volatile std::sig_atomic_t interrupted = 0;

void interrupt(int) { interrupted = 1; }

template<class T>
T number(std::string_view text, T low, T high) {
    T value{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size() || !(value >= low && value <= high)) {
        throw std::runtime_error("invalid numeric argument: " + std::string(text));
    }
    return value;
}

void require_count(std::span<char*> args, std::size_t low, std::size_t high) {
    if (args.size() < low || args.size() > high) throw std::runtime_error("wrong argument count; run resonant help");
}

Encoding encoding(std::string_view name) {
    if (name == "pcm16") return Encoding::pcm16;
    if (name == "pcm24") return Encoding::pcm24;
    if (name == "pcm32") return Encoding::pcm32;
    if (name == "float32") return Encoding::float32;
    if (name == "float64") return Encoding::float64;
    throw std::runtime_error("unknown WAVE encoding");
}

Compilation read_patch(std::string_view path) {
    return compile(read_text(std::filesystem::path(path), max_source_bytes));
}

void summary(const VerifiedProgram& checked) {
    const auto& program = checked.program();
    std::cout << "bytecode_version=" << bytecode_version << " instructions=" << program.instructions.size()
              << " registers=" << program.registers << " states=" << program.states.size()
              << " parameters=" << program.parameters.size() << " state_words=" << checked.state_words()
              << " work_per_frame=" << checked.work_per_frame() << " feedback=" << checked.has_feedback()
              << " left_latency=" << checked.latency()[0] << " right_latency=" << checked.latency()[1] << '\n';
}

void inspect(std::string_view path) {
    const auto c = read_patch(path);
    summary(c.program);
    std::cout << "folded=" << c.folded << " eliminated=" << c.eliminated << '\n';
    Vm vm(c.program, 48000, Execution::scalar);
    (void)vm.event({0, EventType::note_on, 0, 69, 0, 0.7F});
    std::array<float, 256> buffer{};
    const auto stats = vm.render(buffer);
    for (std::size_t pc = 0; pc < c.schedule.size(); ++pc) {
        const auto& row = c.schedule[pc];
        const auto& ins = c.program.program().instructions[pc];
        const auto& contract = opcode(ins.op);
        std::cout << std::setw(4) << pc << "  " << row.span.line << ':' << row.span.column << "  "
                  << row.name << "  " << contract.name << "  " << (ins.rate == Rate::audio ? "audio" : "control");
        if (contract.writes_register) std::cout << "  r" << ins.dst << " through " << row.last_use;
        for (std::size_t i = 0; i < contract.inputs; ++i) std::cout << "  r" << ins.in[i];
        for (const auto& dep : row.dependencies) std::cout << "  " << dep;
        std::cout << '\n';
    }
    std::cout << "registers_at_frame_127";
    for (const auto value : vm.tail_registers()) std::cout << ' ' << value;
    std::cout << "\nexecuted=" << stats.instructions << " peak=" << stats.peak
              << " final_l=" << buffer[254] << " final_r=" << buffer[255] << '\n';
}

void export_audio(const VerifiedProgram& program, const Timeline& timeline, const char* path,
                  std::uint32_t rate, std::size_t block, Encoding format, std::uint64_t source_id,
                  Execution execution) {
    const auto render = render_offline(program, timeline, rate, block, execution);
    const std::vector<double> samples(render.samples.begin(), render.samples.end());
    const auto code = encode(program);
    const auto code_id = name_key({reinterpret_cast<const char*>(code.data()), code.size()});
    std::ostringstream comment;
    comment << "Resonant frames=" << timeline.frames << " source_id=" << std::hex << source_id
            << " program_id=" << code_id;
    const auto identity = comment.str();
    atomic_write(path, encode_wave(samples, {format, rate, 2, 0, identity}));
    std::cout << "frames=" << timeline.frames << " rate=" << rate << " block=" << block
              << " events=" << timeline.events.size() << " instructions=" << render.instructions
              << " clipped=" << render.clipped << " peak=" << render.peak
              << " source_id=" << std::hex << source_id << " program_id=" << code_id << std::dec << '\n';
}

void list_devices() {
    Device device;
    for (const auto& info : device.devices()) {
        std::cout << info.index << ' ' << info.name << " [" << info.host << "]"
                  << " default=" << info.default_output << " native_rate=" << info.native_rate
                  << " suggested_latency_s=" << info.suggested_latency
                  << " rate_44100=" << info.stereo_rates[0] << " rate_48000=" << info.stereo_rates[1]
                  << " rate_96000=" << info.stereo_rates[2] << '\n';
    }
}

void device_run(std::span<char*> args) {
    require_count(args, 2, 7);
    const auto patch = read_patch(args[0]);
    const auto seconds = number<unsigned>(args[1], 1, 28800);
    const auto gain = args.size() >= 3 ? number<float>(args[2], 0, 0.2F) : 0.0F;
    const auto index = args.size() >= 4 ? number<int>(args[3], -1, 10000) : -1;
    const auto rate = args.size() >= 5 ? number<std::uint32_t>(args[4], 8000, 192000) : 48000U;
    const auto block = args.size() >= 6 ? number<std::uint32_t>(args[5], 1, max_block) : 128U;
    std::ofstream trace_file;
    if (args.size() == 7) {
        trace_file.open(args[6], std::ios::out | std::ios::trunc);
        if (!trace_file) throw std::runtime_error("cannot create device trace");
        trace_file << "frame,frames,generation,ns,flags,voices,events,late,clipped,peak\n";
    }
    Engine engine(patch.program, rate);
    for (std::uint8_t n = 24; n < 88; ++n) {
        if (engine.enqueue({0, EventType::note_on, 0, n, 0, 0.2F}) != Enqueue::accepted) {
            throw std::runtime_error("failed to enqueue reference chord");
        }
    }
    Device device;
    device.open(engine, {index, block});
    device.gain(gain);
    std::signal(SIGINT, interrupt);
    std::signal(SIGTERM, interrupt);
    std::array<std::uint64_t, 4096> histogram{};
    std::uint64_t callbacks = 0;
    std::uint64_t total_frames = 0;
    std::uint64_t max_duration = 0;
    std::uint64_t deadline_exceeded = 0;
    std::uint32_t max_voices = 0;
    const auto collect = [&] {
        Trace trace;
        while (engine.poll(trace)) {
            if (trace_file.is_open()) {
                trace_file << trace.first_frame << ',' << trace.frames << ',' << trace.generation << ','
                           << trace.duration_ns << ',' << trace.device_flags << ',' << trace.render.voices << ','
                           << trace.applied_events << ',' << trace.late_events << ',' << trace.render.clipped << ','
                           << trace.render.peak << '\n';
            }
            ++callbacks;
            total_frames += trace.frames;
            max_voices = std::max(max_voices, trace.render.voices);
            max_duration = std::max(max_duration, trace.duration_ns);
            const auto deadline = 1000000000ULL * trace.frames / rate;
            deadline_exceeded += static_cast<std::uint64_t>(trace.duration_ns >= deadline);
            ++histogram[std::min<std::size_t>(trace.duration_ns / 1000, histogram.size() - 1)];
        }
    };
    const auto start = std::chrono::steady_clock::now();
    const auto until = start + std::chrono::seconds(seconds);
    auto last_check = start;
    while (!interrupted && std::chrono::steady_clock::now() < until) {
        collect();
        const auto now = std::chrono::steady_clock::now();
        if (now - last_check >= std::chrono::milliseconds(250)) {
            if (!device.status().active) throw std::runtime_error("output device stopped");
            last_check = now;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }
    device.close();
    const auto status = device.status();
    collect();
    if (trace_file.is_open()) {
        trace_file.close();
        if (!trace_file) throw std::runtime_error("failed to commit device trace");
    }
    const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::uint64_t running = 0;
    std::size_t bucket = 0;
    for (; bucket + 1 < histogram.size(); ++bucket) {
        running += histogram[bucket];
        if (running >= callbacks - callbacks / 1000) break;
    }
    std::cout << "elapsed_seconds=" << elapsed << " callbacks=" << callbacks << " frames=" << total_frames
              << " device_callbacks=" << status.callbacks << " min_block=" << status.smallest_block
              << " max_block=" << status.largest_block << " max_voices=" << max_voices
              << " engine_p999_us_bucket=" << bucket << " engine_max_ns=" << max_duration
              << " engine_deadline_exceeded=" << deadline_exceeded << " underflows=" << status.underflows
              << " trace_lost=" << engine.counters().trace_full << " reported_latency_seconds=" << status.output_latency
              << " muted=" << (gain == 0) << " cancelled=" << interrupted << '\n';
    if (interrupted || callbacks == 0 || status.incompatible_callback || engine.counters().trace_full != 0 ||
        status.callbacks != callbacks || total_frames != engine.clock()) {
        throw std::runtime_error("incomplete device run");
    }
}

void usage() {
    std::cout << "resonant compile PATCH BYTECODE\n"
              << "resonant verify BYTECODE\n"
              << "resonant inspect PATCH\n"
              << "resonant render SESSION WAVE [RATE [BLOCK [ENCODING [scalar|block]]]]\n"
              << "resonant render-patch PATCH WAVE FRAMES [RATE [BLOCK [ENCODING [scalar|block]]]]\n"
              << "resonant schedule SESSION [RATE]\n"
              << "resonant wave-info WAVE\n"
              << "resonant devices\n"
              << "resonant play PATCH SECONDS [GAIN [DEVICE [RATE [BLOCK [TRACE_CSV]]]]]\n"
              << "resonant doctor\n";
}

void command(std::string_view mode, std::span<char*> args) {
    if (mode == "compile") {
        require_count(args, 2, 2);
        const auto compiled = read_patch(args[0]);
        atomic_write(args[1], encode(compiled.program));
        summary(compiled.program);
    } else if (mode == "verify") {
        require_count(args, 1, 1);
        summary(decode(read_file(args[0], max_bytecode_bytes)));
    } else if (mode == "inspect") {
        require_count(args, 1, 1);
        inspect(args[0]);
    } else if (mode == "render" || mode == "render-patch") {
        const std::size_t fields = mode == "render" ? 2 : 3;
        require_count(args, fields, fields + 4);
        const auto rate = args.size() > fields ? number<std::uint32_t>(args[fields], 8000, 192000) : 48000U;
        const auto block = args.size() > fields + 1 ? number<std::size_t>(args[fields + 1], 1, max_block) : 128U;
        const auto format = args.size() > fields + 2 ? encoding(args[fields + 2]) : Encoding::pcm24;
        Execution execution = Execution::block;
        if (args.size() > fields + 3) {
            const std::string_view requested = args[fields + 3];
            if (requested == "scalar") execution = Execution::scalar;
            else if (requested != "block") throw std::runtime_error("unknown execution mode");
        }
        if (mode == "render") {
            const auto bytes = read_text(args[0], max_session_bytes);
            const auto session = parse_session(bytes);
            export_audio(compile(session.source).program, schedule(session, rate), args[1], rate, block, format,
                         name_key(bytes), execution);
        } else {
            const auto frames = number<std::uint64_t>(args[2], 1, max_render_frames);
            Timeline timeline{frames, {{0, EventType::note_on, 0, 69, 0, 0.75F}}};
            if (frames > 1) timeline.events.push_back({3 * frames / 4, EventType::note_off, 0, 69});
            const auto source = read_text(args[0], max_source_bytes);
            export_audio(compile(source).program, timeline, args[1], rate, block, format, name_key(source), execution);
        }
    } else if (mode == "schedule") {
        require_count(args, 1, 2);
        const auto session = parse_session(read_text(args[0], max_session_bytes));
        const auto rate = args.size() == 2 ? number<std::uint32_t>(args[1], 8000, 192000) : 48000U;
        const auto timeline = schedule(session, rate);
        std::cout << "frames," << timeline.frames << '\n' << std::setprecision(std::numeric_limits<float>::max_digits10);
        for (const auto& event : timeline.events) {
            std::cout << event.frame << ',' << static_cast<unsigned>(event.type) << ','
                      << static_cast<unsigned>(event.channel) << ',' << static_cast<unsigned>(event.note) << ','
                      << event.key << ',' << event.value << ',' << event.ramp << '\n';
        }
    } else if (mode == "wave-info") {
        require_count(args, 1, 1);
        const auto bytes = read_file(args[0], 256 * 1024 * 1024);
        const auto wave = parse_wave(bytes);
        std::cout << "frames=" << wave.frames() << " rate=" << wave.rate << " channels=" << wave.channels
                  << " encoding=" << static_cast<unsigned>(wave.encoding) << '\n';
    } else if (mode == "devices") {
        require_count(args, 0, 0);
        list_devices();
    } else if (mode == "doctor") {
        require_count(args, 0, 0);
        std::cout << "simd=" << simd_available() << " portaudio=" << Device::built()
                  << " hardware_threads=" << std::thread::hardware_concurrency() << " binary32="
                  << std::numeric_limits<float>::is_iec559 << '\n';
        if (Device::built()) list_devices();
    } else if (mode == "play") {
        device_run(args);
    } else if (mode == "help") {
        usage();
    } else {
        throw std::runtime_error("unknown command; run resonant help");
    }
}
}

int main(int argc, char** argv) {
    try {
        if (argc == 1) usage();
        else command(argv[1], std::span(argv + 2, static_cast<std::size_t>(argc - 2)));
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "resonant: " << error.what() << '\n';
        return 1;
    }
}
