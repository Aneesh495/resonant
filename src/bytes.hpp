#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <vector>

namespace resonant::binary {
using Bytes = std::vector<std::byte>;

template<class UInt>
UInt read(std::span<const std::byte> bytes, std::size_t pos, std::size_t width = sizeof(UInt)) {
    static_assert(std::is_unsigned_v<UInt>);
    if (width > sizeof(UInt) || pos > bytes.size() || width > bytes.size() - pos) {
        throw std::runtime_error("truncated binary field");
    }
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < width; ++i) {
        value |= std::to_integer<std::uint64_t>(bytes[pos + i]) << (8 * i);
    }
    return static_cast<UInt>(value);
}

template<class UInt>
void write(Bytes& bytes, UInt value, std::size_t width = sizeof(UInt)) {
    static_assert(std::is_unsigned_v<UInt>);
    for (std::size_t i = 0; i < width; ++i) {
        bytes.push_back(static_cast<std::byte>((value >> (8 * i)) & 255U));
    }
}
}
