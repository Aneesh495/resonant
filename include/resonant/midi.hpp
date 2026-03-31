#pragma once

#include "resonant/vm.hpp"

#include <optional>

namespace resonant {
class Midi1 {
public:
    [[nodiscard]] std::optional<Event> push(std::uint8_t byte, std::uint64_t frame) noexcept;
    void reset() noexcept;
    [[nodiscard]] std::uint64_t ignored() const noexcept { return ignored_; }
    [[nodiscard]] std::uint64_t malformed() const noexcept { return malformed_; }

private:
    std::uint8_t status_{};
    std::uint8_t used_{};
    std::uint8_t pending_system_{};
    std::array<std::uint8_t, 2> data_{};
    bool sysex_{};
    std::uint64_t start_{};
    std::uint64_t ignored_{};
    std::uint64_t malformed_{};
};
}
