#include "resonant/engine.hpp"
#include "resonant/graph.hpp"
#include "test.hpp"

#include <atomic>
#include <cmath>
#include <thread>

using namespace resonant;

int main() {
    const auto initial = compile("param v=0; a=audio(v); out(a);");
    Engine engine(initial.program, 48000);
    CHECK(engine.enqueue({4, EventType::parameter, 0, 0, name_key("v"), 0.5F, 4}) == Enqueue::accepted);
    CHECK(engine.enqueue({3, EventType::panic}) == Enqueue::out_of_order);
    CHECK(engine.enqueue({9, EventType::parameter, 0, 0, name_key("missing"), 1}) == Enqueue::accepted);
    std::array<float, 256> output{};
    engine.capture(true);
    engine.render(std::span(output).first(20));
    for (std::size_t i = 0; i < 4; ++i) CHECK(output[2*i] == 0);
    for (std::size_t i = 4; i < 8; ++i) CHECK(output[2*i] == 0.125F * static_cast<float>(i-3));
    CHECK(output[18] == 0.5F && engine.clock() == 10);
    Trace trace;
    CHECK(engine.poll(trace) && trace.first_frame == 0 && trace.applied_events == 2 && trace.absent_parameters == 1);
    CaptureBlock block;
    CHECK(engine.poll_capture(block) && block.first_frame == 0 && block.frames == 10 && block.interleaved[18] == 0.5F);
    engine.capture(false);

    const auto same = compile("param v=0.1; a=audio(v); out(a);");
    const auto louder = compile("param v=0; a=audio(v); b=add(a,0.25); out(b);");
    CHECK(engine.publish(same.program) == 2);
    CHECK(engine.publish(louder.program) == 3);
    engine.render(output);
    CHECK(engine.generation() == 3 && engine.counters().superseded == 1);
    for (std::size_t i = 0; i < 128; ++i) CHECK(output[i*2] == 0.5F + 0.25F * static_cast<float>(i+1)/128);
    CHECK(engine.poll(trace) && trace.fade && trace.generation == 3);
    engine.reclaim();

    for (std::size_t i = 0; i < event_queue_capacity; ++i) {
        CHECK(engine.enqueue({engine.clock(), EventType::panic}) == Enqueue::accepted);
    }
    CHECK(engine.enqueue({engine.clock(), EventType::panic}) == Enqueue::full);
    engine.render(output);
    CHECK(engine.poll(trace) && trace.event_budget_exhausted && trace.applied_events == events_per_callback);
    engine.render(output);
    CHECK(engine.poll(trace) && trace.late_events == events_per_callback);

    Engine concurrent(initial.program, 48000);
    std::atomic<bool> stop{false};
    std::thread audio([&] {
        std::array<float, 256> samples;
        while (!stop.load(std::memory_order_acquire)) {
            concurrent.render(samples);
            for (const auto sample : samples) CHECK(std::isfinite(sample));
        }
    });
    for (int i = 0; i < 4000; ++i) {
        const auto id = concurrent.publish(i % 2 == 0 ? louder.program : same.program);
        while (concurrent.generation() != id) {
            concurrent.reclaim();
            std::this_thread::yield();
        }
        while (concurrent.poll(trace)) {
            if (trace.generation == id) break;
        }
    }
    stop.store(true, std::memory_order_release);
    audio.join();
    CHECK(concurrent.counters().activated == 4000);
}
