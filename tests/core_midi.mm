#include "../src/apple/midi_input.hpp"
#include "resonant/device.hpp"
#include "resonant/graph.hpp"
#include "test.hpp"

#import <CoreMIDI/CoreMIDI.h>
#include <mach/mach_time.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <thread>

using namespace resonant;
using namespace std::chrono_literals;

namespace {
struct VirtualSource {
    MIDIClientRef client{};
    MIDIEndpointRef endpoint{};

    VirtualSource() {
        CHECK(MIDIClientCreateWithBlock(CFSTR("Resonant integration fixture"), &client, nullptr) == noErr);
        CHECK(MIDISourceCreateWithProtocol(client, CFSTR("Resonant virtual integration source"),
                                           kMIDIProtocol_1_0, &endpoint) == noErr);
    }
    ~VirtualSource() {
        (void)MIDIEndpointDispose(endpoint);
        (void)MIDIClientDispose(client);
    }

    void send(std::uint64_t ticks, std::uint32_t word) const {
        MIDIEventList list{};
        auto* packet = MIDIEventListInit(&list, kMIDIProtocol_1_0);
        CHECK(MIDIEventListAdd(&list, sizeof(list), packet, ticks, 1, &word));
        CHECK(MIDIReceivedEventList(endpoint, &list) == noErr);
    }
};
}

int main() {
    const VirtualSource source;
    apple::MidiInput midi;
    CHECK(std::ranges::any_of(midi.sources(), [&](const auto& item) { return item.endpoint == source.endpoint; }));
    midi.connect(source.endpoint);

    constexpr std::uint32_t rate = 48000;
    const auto program = compile("a=poly_sine(.002,.02); g=mul(a,.15); out(g);");
    Engine engine(program.program, rate);
    CHECK(Device::built());
    Device device;
    engine.capture(true);
    device.open(engine, {-1, 128, true});
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    std::optional<FrameAnchor> anchor;
    while (!(anchor = device.clock_anchor()) && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    CHECK(anchor && anchor->block == 128);
    const auto scale = apple::host_seconds_per_tick();
    CHECK(scale > 0);
    const auto base = mach_absolute_time();
    const auto on_ticks = base + static_cast<std::uint64_t>(std::llround(.10 / scale));
    const auto off_ticks = base + static_cast<std::uint64_t>(std::llround(.16 / scale));
    source.send(on_ticks, 0x20903c64);
    source.send(off_ticks, 0x20803c00);

    std::uint64_t first_nonzero = UINT64_MAX;
    std::uint64_t final_event = 0;
    std::uint64_t on_frame = 0;
    unsigned queued = 0;
    unsigned applied = 0;
    unsigned late = 0;
    float peak = 0;
    while ((applied < 2 || first_nonzero == UINT64_MAX) && std::chrono::steady_clock::now() < deadline) {
        Event event;
        while (midi.poll(event)) {
            CHECK(queued < 2 && event.note == 60 && event.channel == 0);
            CHECK(event.frame == (queued == 0 ? on_ticks : off_ticks));
            CHECK(event.type == (queued == 0 ? EventType::note_on : EventType::note_off));
            const auto result = map_host_frame(*anchor, event.frame, scale, rate, engine.clock() + 128, rate / 4, rate / 100);
            CHECK(result.adjustment == ClockAdjustment::exact);
            event.frame = result.frame;
            if (queued == 0) on_frame = event.frame;
            final_event = event.frame;
            CHECK(engine.enqueue(event) == Enqueue::accepted);
            ++queued;
        }
        Trace trace;
        while (engine.poll(trace)) {
            applied += trace.applied_events;
            late += trace.late_events;
            CHECK(trace.render.peak == 0); // Keep the physical monitor muted.
        }
        CaptureBlock block;
        while (engine.poll_capture(block)) {
            for (std::uint32_t i = 0; i < block.frames; ++i) {
                const auto sample = std::abs(block.interleaved[i*2]);
                if (sample > 1e-8F) first_nonzero = std::min(first_nonzero, block.first_frame + i);
                peak = std::max(peak, sample);
            }
        }
        std::this_thread::sleep_for(1ms);
    }
    device.close();
    CHECK(queued == 2 && applied == 2 && late == 0 && midi.dropped() == 0 && engine.capture_overflow() == 0);
    CHECK(final_event - on_frame == 2880);
    CHECK(first_nonzero == on_frame + 1 && peak > .001F);
    std::cout << "Core MIDI virtual source reached the muted hardware callback: on_frame=" << on_frame
              << " first_nonzero=" << first_nonzero << " off_frame=" << final_event
              << " applied=2 late=0 dropped=0\n";
}
