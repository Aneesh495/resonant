#pragma once

#include <cstdint>

namespace resonant {
struct FrameAnchor {
    std::uint64_t ticks{};
    std::uint64_t frame{};
    std::uint32_t block{};
};

enum class ClockAdjustment { exact, late, early };

struct ScheduledFrame {
    std::uint64_t frame;
    ClockAdjustment adjustment;
};

// All timestamps use one monotonic clock. The caller chooses the writable window.
[[nodiscard]] ScheduledFrame map_host_frame(FrameAnchor anchor, std::uint64_t ticks,
                                            double seconds_per_tick, std::uint32_t rate,
                                            std::uint64_t earliest, std::uint32_t lookahead,
                                            std::uint32_t lead) noexcept;
}
