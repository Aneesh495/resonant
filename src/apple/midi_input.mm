#include "midi_input.hpp"
#include "../spsc.hpp"

#import <CoreMIDI/CoreMIDI.h>
#include <mach/mach_time.h>

#include <atomic>
#include <algorithm>
#include <stdexcept>

namespace resonant::apple {
namespace {
void check(OSStatus status, const char* operation) {
    if (status != noErr) throw std::runtime_error(std::string(operation) + ": " + std::to_string(status));
}

struct Inbox {
    Spsc<Event, 512> events;
    std::atomic<std::uint64_t> lost{};
    std::atomic_flag receiving{};
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);

    void receive(const MIDIEventList& list) noexcept {
        if (receiving.test_and_set(std::memory_order_acquire)) {
            lost.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        const auto* packet = &list.packet[0];
        for (std::uint32_t i = 0; i < list.numPackets && i < 64; ++i) {
            decode(*packet);
            if (i + 1 < list.numPackets && i + 1 < 64) packet = MIDIEventPacketNext(packet);
        }
        if (list.numPackets > 64) lost.fetch_add(1, std::memory_order_relaxed);
        receiving.clear(std::memory_order_release);
    }

    void decode(const MIDIEventPacket& packet) noexcept {
        // UMP lengths by message-type nibble, including reserved types.
        constexpr std::array sizes{1U,1U,1U,2U,2U,4U,1U,1U,2U,2U,2U,3U,3U,4U,4U,4U};
        const auto ticks = packet.timeStamp != 0 ? packet.timeStamp : mach_absolute_time();
        const auto width = std::min<std::uint32_t>(packet.wordCount, 64);
        // Core MIDI stores packets as variable-sized arrays. Take the address of the first word
        // because the SDK's fixed-size member is a convenience for making packet lists.
        const auto* words = &packet.words[0];
        for (std::uint32_t i = 0; i < width;) {
            const auto word = words[i];
            const auto type = word >> 28U;
            if (sizes[type] > width - i) {
                lost.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            i += sizes[type];
            if (type != 2 || ((word >> 24U) & 15U) != 0) continue;
            const auto status = static_cast<std::uint8_t>((word >> 16U) & 255U);
            const auto a = static_cast<std::uint8_t>((word >> 8U) & 255U);
            const auto b = static_cast<std::uint8_t>(word & 255U);
            if (status < 0x80 || status >= 0xf0 || a > 127 || b > 127) {
                lost.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            Midi1 parser;
            (void)parser.push(status, ticks);
            (void)parser.push(a, ticks);
            if ((status & 0xe0) == 0xc0) continue; // Program change and channel pressure have one data byte.
            if (const auto event = parser.push(b, ticks); event && !events.push(*event)) {
                lost.fetch_add(1, std::memory_order_relaxed);
            }
        }
        if (packet.wordCount > width) lost.fetch_add(1, std::memory_order_relaxed);
    }
};
}

struct MidiInput::Impl {
    MIDIClientRef client{};
    MIDIPortRef port{};
    std::shared_ptr<Inbox> inbox;
};

MidiInput::MidiInput() : impl_(std::make_unique<Impl>()) {
    check(MIDIClientCreateWithBlock(CFSTR("Resonant"), &impl_->client, nullptr), "open Core MIDI client");
}

MidiInput::~MidiInput() {
    disconnect();
    if (impl_->client != 0) (void)MIDIClientDispose(impl_->client);
}

std::vector<MidiSource> MidiInput::sources() {
    std::vector<MidiSource> result;
    const auto count = MIDIGetNumberOfSources();
    for (ItemCount i = 0; i < count && i < 256; ++i) {
        const auto endpoint = MIDIGetSource(i);
        if (endpoint == 0) continue;
        CFStringRef name = nullptr;
        char bytes[256]{};
        if (MIDIObjectGetStringProperty(endpoint, kMIDIPropertyDisplayName, &name) == noErr && name != nullptr) {
            (void)CFStringGetCString(name, bytes, sizeof(bytes), kCFStringEncodingUTF8);
        }
        if (name != nullptr) CFRelease(name);
        result.push_back({endpoint, bytes[0] != 0 ? bytes : "MIDI input " + std::to_string(i + 1)});
    }
    return result;
}

void MidiInput::connect(std::uint32_t endpoint) {
    disconnect();
    const auto inbox = std::make_shared<Inbox>();
    check(MIDIInputPortCreateWithProtocol(impl_->client, CFSTR("Resonant MIDI 1.0"), kMIDIProtocol_1_0,
                                          &impl_->port, ^(const MIDIEventList* events, void*) {
        if (events != nullptr) inbox->receive(*events);
    }), "create MIDI input port");
    const auto status = MIDIPortConnectSource(impl_->port, endpoint, nullptr);
    if (status != noErr) {
        disconnect();
        check(status, "connect MIDI source");
    }
    impl_->inbox = inbox;
}

void MidiInput::disconnect() noexcept {
    if (impl_->port != 0) (void)MIDIPortDispose(impl_->port);
    impl_->port = 0;
    impl_->inbox.reset();
}

bool MidiInput::poll(Event& event) noexcept { return impl_->inbox && impl_->inbox->events.pop(event); }

std::uint64_t MidiInput::dropped() const noexcept {
    return impl_->inbox ? impl_->inbox->lost.load(std::memory_order_relaxed) : 0;
}

double host_seconds_per_tick() noexcept {
    mach_timebase_info_data_t scale{};
    if (mach_timebase_info(&scale) != KERN_SUCCESS || scale.denom == 0) return 0;
    return (1e-9 * scale.numer) / scale.denom;
}
}
