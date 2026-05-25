#include "resonant/engine.hpp"
#include "resonant/graph.hpp"
#include "test.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <new>
#include <random>
#include <thread>

namespace {
thread_local bool callback = false;
std::atomic<std::uint64_t> observed{};

void mark() noexcept {
    if (callback) observed.fetch_add(1, std::memory_order_relaxed);
}
}

void* operator new(std::size_t size) {
    mark();
    if (auto* p = std::malloc(size == 0 ? 1 : size)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* ptr) noexcept { mark(); std::free(ptr); }
void operator delete[](void* ptr) noexcept { ::operator delete(ptr); }
void operator delete(void* ptr, std::size_t) noexcept { ::operator delete(ptr); }
void operator delete[](void* ptr, std::size_t) noexcept { ::operator delete(ptr); }
void* operator new(std::size_t size, std::align_val_t alignment) {
    mark();
    void* ptr = nullptr;
    if (posix_memalign(&ptr, static_cast<std::size_t>(alignment), size == 0 ? 1 : size) == 0) return ptr;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size, std::align_val_t alignment) { return ::operator new(size, alignment); }
void operator delete(void* ptr, std::align_val_t) noexcept { ::operator delete(ptr); }
void operator delete[](void* ptr, std::align_val_t) noexcept { ::operator delete(ptr); }
void operator delete(void* ptr, std::size_t, std::align_val_t) noexcept { ::operator delete(ptr); }
void operator delete[](void* ptr, std::size_t, std::align_val_t) noexcept { ::operator delete(ptr); }

using namespace resonant;
using Clock = std::chrono::steady_clock;

int main(int argc, char** argv) {
    CHECK(argc == 2);
    const std::string_view mode = argv[1];
    CHECK(mode == "swap" || mode == "audit");
    const auto trials = mode == "swap" ? 100000U : 2000U;
    const auto first = compile("param gain=.4; v=poly_sine(.003,.14); f=lowpass(v,1400,.8); "
                               "p=align(f,8); o=mul(p,gain); out(o);");
    const auto second = compile("param gain=.4; v=poly_sine(.003,.14); f=lowpass(v,950,.9); "
                                "p=align(f,12); o=mul(p,gain); out(o);");
    Engine engine(first.program, 48000);
    CHECK(engine.enqueue({0, EventType::note_on, 0, 55, 0, 0.75F}) == Enqueue::accepted);
    engine.capture(true);

    callback = true;
    auto* volatile probe = ::operator new(19);
    ::operator delete(probe);
    callback = false;
    CHECK(observed.exchange(0) == 2);

    const auto begin = Clock::now();
    const auto deadline = begin + std::chrono::seconds(150);
    std::atomic<bool> stop{};
    std::atomic<std::uint64_t> completed_frames{};
    std::thread audio([&] {
        std::array<float, 2 * max_block> samples{};
        std::mt19937 rng(0x1fbed3);
        std::uint64_t expected = 0;
        while (!stop.load(std::memory_order_acquire)) {
            const auto frames = 8 + rng() % 124;
            const auto out = std::span(samples).first(2 * frames);
            callback = true;
            engine.render(out);
            callback = false;
            for (const auto sample : out) CHECK(std::isfinite(sample) && std::abs(sample) <= 1);
            expected += frames;
            CHECK(engine.clock() == expected);
            if (rng() % 11 == 0) std::this_thread::yield();
        }
        completed_frames.store(expected, std::memory_order_release);
    });
    std::mt19937 control(0x1fbed3 + 1);
    Trace trace;
    CaptureBlock capture;
    std::uint64_t capture_end = 0;
    std::uint64_t traces = 0;
    std::uint64_t trace_gaps = 0;
    std::uint64_t trace_end = 0;
    std::uint32_t max_voices = 0;
    const auto drain = [&] {
        engine.reclaim();
        while (engine.poll(trace)) {
            CHECK(trace.first_frame >= trace_end);
            trace_gaps += trace.first_frame - trace_end;
            trace_end = trace.first_frame + trace.frames;
            ++traces;
            max_voices = std::max(max_voices, trace.render.voices);
        }
        while (engine.poll_capture(capture)) {
            CHECK(capture.first_frame >= capture_end);
            capture_end = capture.first_frame + capture.frames;
        }
    };
    for (std::uint32_t i = 0; i < trials; ++i) {
        const auto id = engine.publish((i & 1U) == 0 ? second.program : first.program);
        while (engine.generation() != id) {
            CHECK(Clock::now() < deadline);
            drain();
            std::this_thread::yield();
        }
        if ((i % 32) == 0) {
            const auto value = static_cast<float>(control() % 100) / 100;
            CHECK(engine.enqueue({engine.clock() + 64, EventType::parameter, 0, 0,
                                  name_key("gain"), value, 200}) == Enqueue::accepted);
        }
        if (control() % 17 == 0) std::this_thread::yield();
        drain();
    }
    stop.store(true, std::memory_order_release);
    audio.join();
    drain();
    const auto counters = engine.counters();
    CHECK(counters.activated == trials && counters.migration_misses == 0 && counters.superseded == 0);
    CHECK(engine.clock() == completed_frames.load() && engine.clock() > trials && max_voices == 1);
    CHECK(observed.load() == 0);
    std::cout << "swaps=" << counters.activated << " frames=" << engine.clock() << " trace_records=" << traces
              << " trace_dropped=" << counters.trace_full << " missing_trace_frames=" << trace_gaps
              << " capture_dropped=" << engine.capture_overflow() << " cpp_heap_events=" << observed.load()
              << " elapsed_seconds=" << std::chrono::duration<double>(Clock::now()-begin).count() << '\n';
}
