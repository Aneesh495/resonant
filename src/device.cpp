#include "resonant/device.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <stdexcept>

#if RESONANT_PORTAUDIO
#include <portaudio.h>
#endif

namespace resonant {
namespace {
#if RESONANT_PORTAUDIO
void checked(PaError result, const char* operation) {
    if (result < 0) throw std::runtime_error(std::string(operation) + ": " + Pa_GetErrorText(result));
}

PaStreamParameters output_for(PaDeviceIndex index, const PaDeviceInfo& info) {
    return {index, 2, paFloat32, info.defaultLowOutputLatency, nullptr};
}
#else
[[noreturn]] void missing() { throw std::runtime_error("this build does not include PortAudio"); }
#endif
}

struct Device::Impl {
#if RESONANT_PORTAUDIO
    PaStream* stream{};
    Engine* engine{};
    double latency{};
    std::atomic<float> gain{};
    std::atomic<std::uint64_t> callbacks{};
    std::atomic<std::uint64_t> underflows{};
    std::atomic<std::uint32_t> smallest{};
    std::atomic<std::uint32_t> largest{};
    std::atomic<bool> invalid{};

    static_assert(std::atomic<float>::is_always_lock_free && std::atomic<std::uint32_t>::is_always_lock_free &&
                  std::atomic<std::uint64_t>::is_always_lock_free && std::atomic<bool>::is_always_lock_free);

    static int callback(const void*, void* output, unsigned long frames,
                        const PaStreamCallbackTimeInfo*, PaStreamCallbackFlags flags, void* data) noexcept {
        auto& state = *static_cast<Impl*>(data);
        if (output == nullptr || frames == 0 || frames > max_block) {
            state.invalid.store(true, std::memory_order_release);
            return paAbort;
        }
        const auto n = static_cast<std::uint32_t>(frames);
        const auto min = state.smallest.load(std::memory_order_relaxed);
        if (min == 0 || n < min) state.smallest.store(n, std::memory_order_relaxed);
        if (n > state.largest.load(std::memory_order_relaxed)) state.largest.store(n, std::memory_order_relaxed);
        state.engine->render({static_cast<float*>(output), 2 * n}, static_cast<std::uint32_t>(flags),
                             state.gain.load(std::memory_order_relaxed));
        state.callbacks.fetch_add(1, std::memory_order_relaxed);
        if ((flags & paOutputUnderflow) != 0) state.underflows.fetch_add(1, std::memory_order_relaxed);
        return paContinue;
    }
#endif
};

bool Device::built() noexcept { return RESONANT_PORTAUDIO != 0; }

Device::Device() : impl_(std::make_unique<Impl>()) {
#if RESONANT_PORTAUDIO
    checked(Pa_Initialize(), "initialize PortAudio");
#endif
}

Device::~Device() {
    close();
#if RESONANT_PORTAUDIO
    (void)Pa_Terminate();
#endif
}

std::vector<DeviceInfo> Device::devices() const {
#if RESONANT_PORTAUDIO
    const auto count = Pa_GetDeviceCount();
    checked(count, "enumerate audio devices");
    const auto default_device = Pa_GetDefaultOutputDevice();
    std::vector<DeviceInfo> devices;
    for (int i = 0; i < count; ++i) {
        const auto* info = Pa_GetDeviceInfo(i);
        if (info == nullptr || info->maxOutputChannels < 2) continue;
        const auto* host = Pa_GetHostApiInfo(info->hostApi);
        const auto out = output_for(i, *info);
        DeviceInfo entry{i, info->name, host == nullptr ? "unknown" : host->name, info->defaultSampleRate,
                         info->defaultLowOutputLatency, i == default_device, {}};
        constexpr std::array rates{44100, 48000, 96000};
        for (std::size_t r = 0; r < rates.size(); ++r) {
            entry.stereo_rates[r] = Pa_IsFormatSupported(nullptr, &out, rates[r]) == paFormatIsSupported;
        }
        devices.push_back(std::move(entry));
    }
    return devices;
#else
    missing();
#endif
}

void Device::open(Engine& engine, DeviceConfig config) {
#if RESONANT_PORTAUDIO
    if (config.block_size < 1 || config.block_size > max_block) throw std::runtime_error("invalid device block size");
    const auto index = config.device == -1 ? Pa_GetDefaultOutputDevice() : config.device;
    const auto* info = Pa_GetDeviceInfo(index);
    if (info == nullptr || info->maxOutputChannels < 2) throw std::runtime_error("no suitable stereo output device");
    const auto out = output_for(index, *info);
    const double rate = engine.sample_rate();
    checked(Pa_IsFormatSupported(nullptr, &out, rate), "negotiate stereo output sample rate");

    close();
    impl_->engine = &engine;
    impl_->gain.store(0, std::memory_order_relaxed);
    impl_->callbacks.store(0, std::memory_order_relaxed);
    impl_->underflows.store(0, std::memory_order_relaxed);
    impl_->smallest.store(0, std::memory_order_relaxed);
    impl_->largest.store(0, std::memory_order_relaxed);
    impl_->invalid.store(false, std::memory_order_relaxed);
    checked(Pa_OpenStream(&impl_->stream, nullptr, &out, rate, config.block_size, paNoFlag, &Impl::callback, impl_.get()),
            "open stereo output");
    try {
        const auto* details = Pa_GetStreamInfo(impl_->stream);
        if (details == nullptr || details->sampleRate != rate) throw std::runtime_error("device returned a different rate");
        impl_->latency = details->outputLatency;
        checked(Pa_StartStream(impl_->stream), "start stereo output");
    } catch (...) {
        close();
        throw;
    }
#else
    (void)engine;
    (void)config;
    missing();
#endif
}

void Device::close() noexcept {
#if RESONANT_PORTAUDIO
    if (impl_->stream == nullptr) return;
    (void)Pa_AbortStream(impl_->stream);
    (void)Pa_CloseStream(impl_->stream);
    impl_->stream = nullptr;
    impl_->engine = nullptr;
#endif
}

DeviceStatus Device::status() const {
#if RESONANT_PORTAUDIO
    PaError active = 0;
    if (impl_->stream != nullptr) {
        active = Pa_IsStreamActive(impl_->stream);
        checked(active, "query stream status");
    }
    return {active == 1, impl_->invalid.load(std::memory_order_acquire),
            impl_->callbacks.load(std::memory_order_relaxed), impl_->underflows.load(std::memory_order_relaxed),
            impl_->latency, impl_->smallest.load(std::memory_order_relaxed), impl_->largest.load(std::memory_order_relaxed)};
#else
    missing();
#endif
}

void Device::gain(float value) noexcept {
#if RESONANT_PORTAUDIO
    impl_->gain.store(std::isfinite(value) ? std::clamp(value, 0.0F, 0.2F) : 0.0F, std::memory_order_relaxed);
#else
    (void)value;
#endif
}
}
