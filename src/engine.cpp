#include "resonant/engine.hpp"

#include "fp_mode.hpp"

#include "spsc.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace resonant {
namespace {
using Clock = std::chrono::steady_clock;
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
static_assert(std::atomic<bool>::is_always_lock_free);

void combine(RenderStats& to, RenderStats from) noexcept {
    to.instructions += from.instructions;
    to.clipped += from.clipped;
    to.peak = std::max(to.peak, from.peak);
    to.voices = from.voices;
}
}

struct Engine::Impl {
    struct Package {
        Vm vm;
        std::uint64_t generation;
        const std::uint64_t key;
        std::uint64_t base{};
        Vm::Migration migration{};

        Package(const VerifiedProgram& program, std::uint32_t rate, Execution execution, std::uint64_t id)
            : vm(program, rate, execution), generation(id), key(program_key(program)) {}
    };

    static_assert(std::atomic<Package*>::is_always_lock_free);

    const std::uint32_t rate;
    const Execution execution;
    // Pending is control to audio. Retired is audio to control.
    std::atomic<Package*> pending{};
    std::atomic<Package*> retired{};
    std::atomic<Package*> visible{};
    Package* current;
    Package* fading{};
    std::uint32_t faded{};
    std::uint64_t next_generation = 1;
    std::uint64_t last_accepted{};
    std::uint64_t cursor{};
    std::atomic<std::uint64_t> completed{};
    std::atomic<std::uint64_t> generation{1};
    std::atomic<bool> emergency{};
    std::atomic<bool> cancel{};
    std::atomic<std::uint64_t> cancelled_events{};
    std::atomic<bool> capture_enabled{};
    Spsc<Event, event_queue_capacity> events;
    Spsc<Trace, 512> traces;
    Spsc<CaptureBlock, 512> captured;
    std::array<float, max_block * 2> fade_buffer{};
    std::atomic<std::uint64_t> queue_full{};
    std::atomic<std::uint64_t> queue_invalid{};
    std::atomic<std::uint64_t> queue_order{};
    std::atomic<std::uint64_t> trace_full{};
    std::atomic<std::uint64_t> capture_full{};
    std::atomic<std::uint64_t> superseded{};
    std::atomic<std::uint64_t> activated{};
    std::atomic<std::uint64_t> migration_misses{};

    Impl(const VerifiedProgram& program, std::uint32_t sample_rate, Execution mode)
        : rate(sample_rate), execution(mode), current(new Package(program, sample_rate, mode, 1)) {
        visible.store(current, std::memory_order_relaxed);
    }

    ~Impl() {
        delete pending.load(std::memory_order_relaxed);
        delete retired.load(std::memory_order_relaxed);
        delete fading;
        delete current;
    }

    void activate() noexcept {
        if (fading != nullptr || retired.load(std::memory_order_acquire) != nullptr) return;
        auto* next = pending.exchange(nullptr, std::memory_order_acq_rel);
        if (next == nullptr) return;
        if (next->base == current->generation) {
            next->vm.migrate(current->vm, next->migration);
        } else {
            migration_misses.fetch_add(1, std::memory_order_relaxed);
        }
        fading = current;
        faded = 0;
        current = next;
        visible.store(current, std::memory_order_release);
        generation.store(current->generation, std::memory_order_release);
        activated.fetch_add(1, std::memory_order_relaxed);
    }

    bool apply(const Event& e) noexcept {
        const bool applied = current->vm.event(e);
        if (fading != nullptr) (void)fading->vm.event(e);
        return applied;
    }

    void segment(std::span<float> out, Trace& trace) noexcept {
        if (out.empty()) return;
        combine(trace.render, current->vm.render(out));
        if (fading == nullptr) return;
        trace.fade = true;
        const auto overlap = std::min<std::size_t>(out.size() / 2, swap_fade_frames - faded);
        auto old = std::span(fade_buffer).first(2 * overlap);
        const auto old_stats = fading->vm.render(old);
        trace.render.instructions += old_stats.instructions;
        trace.render.clipped += old_stats.clipped;
        for (std::size_t i = 0; i < overlap; ++i) {
            const auto weight = static_cast<float>(faded + i + 1) / swap_fade_frames;
            for (std::size_t channel = 0; channel < 2; ++channel) {
                const auto lane = 2 * i + channel;
                out[lane] = old[lane] * (1.0F - weight) + out[lane] * weight;
            }
        }
        faded += static_cast<std::uint32_t>(overlap);
        if (faded == swap_fade_frames) {
            auto* previous = fading;
            fading = nullptr;
            retired.store(previous, std::memory_order_release);
        }
    }
};

Engine::Engine(const VerifiedProgram& initial, std::uint32_t rate, Execution execution)
    : impl_(std::make_unique<Impl>(initial, rate, execution)) {}

Engine::~Engine() = default;

std::uint64_t Engine::publish(const VerifiedProgram& program) {
    reclaim();
    if (impl_->next_generation == std::numeric_limits<std::uint64_t>::max()) {
        throw std::runtime_error("generation counter exhausted");
    }
    const auto id = impl_->next_generation + 1;
    auto next = std::make_unique<Impl::Package>(program, impl_->rate, impl_->execution, id);
    const auto* base = impl_->visible.load(std::memory_order_acquire);
    next->base = base->generation;
    next->migration = next->vm.migration_from(base->vm);
    impl_->next_generation = id;
    auto* superseded = impl_->pending.exchange(next.release(), std::memory_order_acq_rel);
    if (superseded != nullptr) {
        impl_->superseded.fetch_add(1, std::memory_order_relaxed);
        delete superseded;
    }
    return id;
}

Enqueue Engine::enqueue(const Event& event) noexcept {
    if (cancellation_pending()) return Enqueue::cancellation_pending;
    if (!valid_event(event)) {
        impl_->queue_invalid.fetch_add(1, std::memory_order_relaxed);
        return Enqueue::invalid;
    }
    if (event.frame < impl_->last_accepted) {
        impl_->queue_order.fetch_add(1, std::memory_order_relaxed);
        return Enqueue::out_of_order;
    }
    if (!impl_->events.push(event)) {
        impl_->queue_full.fetch_add(1, std::memory_order_relaxed);
        return Enqueue::full;
    }
    impl_->last_accepted = event.frame;
    return Enqueue::accepted;
}

void Engine::panic() noexcept { impl_->emergency.store(true, std::memory_order_release); }

void Engine::cancel_events() noexcept {
    impl_->last_accepted = 0;
    impl_->cancel.store(true, std::memory_order_release);
}

bool Engine::cancellation_pending() const noexcept { return impl_->cancel.load(std::memory_order_acquire); }

void Engine::reclaim() noexcept { delete impl_->retired.exchange(nullptr, std::memory_order_acq_rel); }

bool Engine::poll(Trace& trace) noexcept { return impl_->traces.pop(trace); }

void Engine::capture(bool enabled) noexcept { impl_->capture_enabled.store(enabled, std::memory_order_release); }

bool Engine::poll_capture(CaptureBlock& block) noexcept { return impl_->captured.pop(block); }

std::uint64_t Engine::capture_overflow() const noexcept { return impl_->capture_full.load(std::memory_order_relaxed); }

std::uint32_t Engine::sample_rate() const noexcept { return impl_->rate; }

std::uint64_t Engine::clock() const noexcept { return impl_->completed.load(std::memory_order_acquire); }

std::uint64_t Engine::generation() const noexcept { return impl_->generation.load(std::memory_order_acquire); }

EngineCounters Engine::counters() const noexcept {
    return {impl_->queue_full.load(std::memory_order_relaxed), impl_->queue_invalid.load(std::memory_order_relaxed),
            impl_->queue_order.load(std::memory_order_relaxed), impl_->trace_full.load(std::memory_order_relaxed),
            impl_->superseded.load(std::memory_order_relaxed), impl_->activated.load(std::memory_order_relaxed),
            impl_->migration_misses.load(std::memory_order_relaxed), impl_->cancelled_events.load(std::memory_order_relaxed)};
}

void Engine::render(std::span<float> out, std::uint32_t device_flags, float gain) noexcept {
    const auto begin = Clock::now();
    [[maybe_unused]] const numeric::FloatMode mode;
    const auto frames = out.size() / 2;
    if (frames == 0 || frames > max_block || out.size() % 2 != 0) return;
    if (frames > std::numeric_limits<std::uint64_t>::max() - impl_->cursor) {
        std::ranges::fill(out, 0.0F);
        return;
    }
    impl_->activate();
    const auto capture = impl_->capture_enabled.load(std::memory_order_acquire);
    // Retire may release the old package during this block. Read its identity before rendering.
    const auto fade_frames = capture && impl_->fading != nullptr
        ? std::min<std::size_t>(frames, swap_fade_frames - impl_->faded) : 0;
    const auto fading_key = fade_frames != 0 ? impl_->fading->key : 0;
    if (impl_->cancel.load(std::memory_order_acquire)) {
        impl_->cancelled_events.fetch_add(impl_->events.discard_all(), std::memory_order_relaxed);
        (void)impl_->apply({impl_->cursor, EventType::panic});
        impl_->cancel.store(false, std::memory_order_release);
    }
    if (impl_->emergency.exchange(false, std::memory_order_acq_rel)) {
        (void)impl_->apply({impl_->cursor, EventType::panic});
    }
    Trace trace{};
    trace.first_frame = impl_->cursor;
    trace.generation = impl_->current->generation;
    trace.frames = static_cast<std::uint32_t>(frames);
    trace.device_flags = device_flags;
    const auto end = impl_->cursor + frames;
    std::size_t rendered = 0;
    Event event{};
    while (trace.applied_events < events_per_callback && impl_->events.peek(event) && event.frame < end) {
        const auto offset = event.frame <= impl_->cursor ? 0 : static_cast<std::size_t>(event.frame - impl_->cursor);
        impl_->segment(out.subspan(2 * rendered, 2 * (offset - rendered)), trace);
        rendered = offset;
        if (!impl_->apply(event)) ++trace.absent_parameters;
        impl_->events.consume();
        ++trace.applied_events;
        trace.late_events += static_cast<std::uint32_t>(event.frame < impl_->cursor);
    }
    trace.event_budget_exhausted = impl_->events.peek(event) && event.frame < end;
    impl_->segment(out.subspan(2 * rendered), trace);
    impl_->cursor = end;

    if (capture) {
        CaptureBlock block{};
        block.first_frame = trace.first_frame;
        block.generation = trace.generation;
        block.program = impl_->current->key;
        block.fading_program = fading_key;
        block.frames = trace.frames;
        block.fade_frames = static_cast<std::uint32_t>(fade_frames);
        std::ranges::copy(out, block.interleaved.begin());
        if (!impl_->captured.push(block)) impl_->capture_full.fetch_add(1, std::memory_order_relaxed);
    }

    trace.scope_size = static_cast<std::uint32_t>(std::min(trace.scope.size(), frames));
    for (std::size_t i = 0; i < trace.scope_size; ++i) trace.scope[i] = out[2 * i * frames / trace.scope_size];
    gain = std::isfinite(gain) ? std::clamp(gain, 0.0F, 1.0F) : 0.0F;
    trace.render.peak = 0;
    for (auto& sample : out) {
        sample *= gain;
        trace.render.peak = std::max(trace.render.peak, std::abs(sample));
    }
    impl_->completed.store(end, std::memory_order_release);
    trace.duration_ns = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - begin).count());
    if (!impl_->traces.push(trace)) impl_->trace_full.fetch_add(1, std::memory_order_relaxed);
}
}
