#pragma once

#include "resonant/vm.hpp"

namespace resonant {
inline constexpr std::uint32_t event_queue_capacity = 4096;
inline constexpr std::uint32_t events_per_callback = 128;
inline constexpr std::uint32_t swap_fade_frames = 128;

enum class Enqueue { accepted, invalid, out_of_order, full, cancellation_pending };

struct Trace {
    std::uint64_t first_frame{};
    std::uint64_t generation{};
    std::uint64_t duration_ns{};
    std::uint32_t frames{};
    std::uint32_t device_flags{};
    std::uint32_t applied_events{};
    std::uint32_t late_events{};
    std::uint32_t absent_parameters{};
    bool event_budget_exhausted{};
    bool fade{};
    RenderStats render;
    std::array<float, 64> scope{};
    std::uint32_t scope_size{};
};

struct EngineCounters {
    std::uint64_t queue_full{};
    std::uint64_t queue_invalid{};
    std::uint64_t queue_order{};
    std::uint64_t trace_full{};
    std::uint64_t superseded{};
    std::uint64_t activated{};
    std::uint64_t migration_misses{};
    std::uint64_t cancelled_events{};
};

struct CaptureBlock {
    std::uint64_t first_frame{};
    std::uint64_t generation{};
    std::uint64_t program{};
    std::uint64_t fading_program{};
    std::uint32_t frames{};
    std::uint32_t fade_frames{};
    std::array<float, max_block * 2> interleaved{};
};

class Engine {
public:
    Engine(const VerifiedProgram& initial, std::uint32_t sample_rate, Execution execution = Execution::block);
    // Destruction requires the callback to have stopped.
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    // Exactly one control producer and one audio consumer may operate concurrently.
    [[nodiscard]] std::uint64_t publish(const VerifiedProgram& program);
    [[nodiscard]] Enqueue enqueue(const Event& event) noexcept;
    void panic() noexcept;
    void cancel_events() noexcept;
    [[nodiscard]] bool cancellation_pending() const noexcept;
    void reclaim() noexcept;
    [[nodiscard]] bool poll(Trace& trace) noexcept;
    void capture(bool enabled) noexcept;
    [[nodiscard]] bool poll_capture(CaptureBlock& block) noexcept;
    [[nodiscard]] std::uint64_t capture_overflow() const noexcept;

    [[nodiscard]] std::uint32_t sample_rate() const noexcept;
    [[nodiscard]] std::uint64_t clock() const noexcept;
    [[nodiscard]] std::uint64_t generation() const noexcept;
    [[nodiscard]] EngineCounters counters() const noexcept;

    // Caller supplies a stereo buffer of at most max_block frames.
    void render(std::span<float> interleaved, std::uint32_t device_flags = 0, float gain = 1) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
