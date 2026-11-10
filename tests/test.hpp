#pragma once

#include <cstdlib>
#include <iostream>
#include <stdexcept>

#define CHECK(condition) do { \
    if (!(condition)) { \
        std::cerr << __FILE__ << ':' << __LINE__ << ": " << #condition << '\n'; \
        std::exit(1); \
    } \
} while (false)

template<class F>
void rejects(F action) {
    try {
        action();
    } catch (const std::runtime_error&) {
        return;
    }
    CHECK(false);
}
