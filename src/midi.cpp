#include "resonant/midi.hpp"

namespace resonant {
std::optional<Event> Midi1::push(std::uint8_t byte, std::uint64_t frame) noexcept {
    if (byte >= 0xf8) {
        ++ignored_;
        return {};
    }
    if ((byte & 0x80) != 0) {
        if (used_ != 0 || pending_system_ != 0 || (sysex_ && byte != 0xf7)) ++malformed_;
        used_ = 0;
        pending_system_ = 0;
        sysex_ = byte == 0xf0;
        status_ = byte < 0xf0 ? byte : 0;
        start_ = frame;
        if (byte == 0xf1 || byte == 0xf3) pending_system_ = 1;
        if (byte == 0xf2) pending_system_ = 2;
        if (status_ == 0) ++ignored_;
        return {};
    }
    if (sysex_) return {};
    if (pending_system_ != 0) {
        --pending_system_;
        return {};
    }
    if (status_ == 0) {
        ++malformed_;
        return {};
    }
    const auto kind = status_ & 0xf0;
    if (used_ == 0) start_ = frame;
    data_[used_++] = byte;
    const auto width = kind == 0xc0 || kind == 0xd0 ? 1 : 2;
    if (used_ < width) return {};
    used_ = 0;

    Event event{start_, EventType::note_off, static_cast<std::uint8_t>(status_ & 15), data_[0]};
    if (kind == 0x80 || kind == 0x90) {
        if (kind == 0x90 && data_[1] != 0) event.type = EventType::note_on;
        event.value = static_cast<float>(data_[1]) / 127.0F;
        return event;
    }
    if (kind == 0xb0 && (data_[0] == 64 || data_[0] == 123 || data_[0] == 120)) {
        if (data_[0] == 64) {
            event.type = EventType::sustain;
            event.value = data_[1] >= 64 ? 1.0F : 0.0F;
        } else {
            event.type = data_[0] == 120 ? EventType::all_sound_off : EventType::all_off;
        }
        return event;
    }
    ++ignored_;
    return {};
}

void Midi1::reset() noexcept {
    status_ = 0;
    used_ = 0;
    pending_system_ = 0;
    sysex_ = false;
}
}
