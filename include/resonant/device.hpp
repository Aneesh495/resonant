#pragma once

#include "resonant/engine.hpp"

#include <string>

namespace resonant {
struct DeviceInfo {
    int index;
    std::string name;
    std::string host;
    double native_rate;
    double suggested_latency;
    bool default_output;
    std::array<bool, 3> stereo_rates; // 44100, 48000, 96000
};

struct DeviceConfig {
    int device = -1;
    std::uint32_t block_size = 128;
};

struct DeviceStatus {
    bool active;
    bool incompatible_callback;
    std::uint64_t callbacks;
    std::uint64_t underflows;
    double output_latency;
    std::uint32_t smallest_block;
    std::uint32_t largest_block;
};

class Device {
public:
    Device();
    ~Device();
    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;

    [[nodiscard]] static bool built() noexcept;
    [[nodiscard]] std::vector<DeviceInfo> devices() const;
    void open(Engine& engine, DeviceConfig config = {});
    void close() noexcept;
    [[nodiscard]] DeviceStatus status() const;
    // Default is muted. The device boundary caps gain at 0.2 (-14 dBFS).
    void gain(float value) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
