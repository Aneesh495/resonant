#include "resonant/host_clock.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace resonant {
ScheduledFrame map_host_frame(FrameAnchor anchor, std::uint64_t ticks, double seconds_per_tick,
                              std::uint32_t rate, std::uint64_t earliest, std::uint32_t lookahead,
                              std::uint32_t lead) noexcept {
    const auto latest = earliest + std::min<std::uint64_t>(lookahead, UINT64_MAX - earliest);
    if (!(seconds_per_tick > 0) || !std::isfinite(seconds_per_tick) || rate == 0) {
        return {earliest, ClockAdjustment::late};
    }
    // Subtract before conversion so a long-running host retains sub-sample precision.
    const auto magnitude = ticks >= anchor.ticks ? ticks - anchor.ticks : anchor.ticks - ticks;
    const auto tick_delta = static_cast<long double>(magnitude) * (ticks >= anchor.ticks ? 1 : -1);
    const auto frame_delta = std::round(tick_delta * seconds_per_tick * rate + lead);
    if (frame_delta < 0) {
        if (frame_delta <= -0x1p64L || frame_delta < -static_cast<long double>(anchor.frame)) return {earliest, ClockAdjustment::late};
    } else if (frame_delta > static_cast<long double>(latest) ||
               frame_delta >= 0x1p64L) {
        return {latest, ClockAdjustment::early};
    }
    const auto count = static_cast<std::uint64_t>(std::abs(frame_delta));
    if (frame_delta >= 0 && count > UINT64_MAX - anchor.frame) return {latest, ClockAdjustment::early};
    const auto target = frame_delta < 0 ? anchor.frame - count : anchor.frame + count;
    if (target < earliest) return {earliest, ClockAdjustment::late};
    if (target > latest) return {latest, ClockAdjustment::early};
    return {target, ClockAdjustment::exact};
}
}
