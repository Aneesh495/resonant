#pragma once

#include <charconv>
#include <cmath>
#include <locale>
#include <optional>
#include <sstream>
#include <string_view>
#include <type_traits>

namespace resonant {
template<class T>
std::optional<T> decimal(std::string_view source) {
    if (source.empty() || source.size() > 63) return {};
    T value{};
    if constexpr (std::is_floating_point_v<T>) {
        // Older libc++ distributions have integral from_chars only.
        std::istringstream input{std::string(source)};
        input.imbue(std::locale::classic());
        if (!(input >> std::noskipws >> value) || !input.eof() || !std::isfinite(value)) return {};
    } else {
        const auto [end, error] = std::from_chars(source.data(), source.data() + source.size(), value);
        if (error != std::errc{} || end != source.data() + source.size()) return {};
    }
    return value;
}
}
