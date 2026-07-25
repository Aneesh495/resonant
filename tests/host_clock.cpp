#include "resonant/host_clock.hpp"
#include "test.hpp"

using namespace resonant;

int main() {
    // 24 MHz is one common Core MIDI host tick rate.
    constexpr auto hz = 24000000.0;
    constexpr FrameAnchor anchor{0x1000000000000000ULL, 720000, 128};
    for (const auto rate : {44100U, 48000U, 96000U}) {
        const auto x = map_host_frame(anchor, anchor.ticks + 240000, 1.0 / hz, rate, 720128, 25000, 256);
        CHECK(x.frame == anchor.frame + rate / 100 + 256 && x.adjustment == ClockAdjustment::exact);
        const auto y = map_host_frame(anchor, anchor.ticks - 240000, 1.0 / hz, rate, 719000, 25000, 256);
        CHECK(y.frame == anchor.frame - rate / 100 + 256 && y.adjustment == ClockAdjustment::exact);
    }
    const auto late = map_host_frame(anchor, anchor.ticks, 1.0 / hz, 48000, 720128, 25000, 0);
    CHECK(late.frame == 720128 && late.adjustment == ClockAdjustment::late);
    const auto early = map_host_frame(anchor, UINT64_MAX, 1.0 / hz, 48000, 720128, 25000, 0);
    CHECK(early.frame == 745128 && early.adjustment == ClockAdjustment::early);
    const auto near_end = map_host_frame({anchor.ticks, UINT64_MAX - 20, 128},
                                        anchor.ticks + 240000, 1.0 / hz, 48000, UINT64_MAX - 5, 25000, 256);
    CHECK(near_end.frame == UINT64_MAX && near_end.adjustment == ClockAdjustment::early);
    const auto near_start = map_host_frame({anchor.ticks, 20, 128},
                                          anchor.ticks - 240000, 1.0 / hz, 48000, 20, 25000, 0);
    CHECK(near_start.frame == 20 && near_start.adjustment == ClockAdjustment::late);
}
