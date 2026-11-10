#include "resonant/session.hpp"

#include <cerrno>
#include <fstream>
#include <limits>
#include <system_error>
#include <unistd.h>

namespace resonant {
std::vector<std::byte> read_file(const std::filesystem::path& path, std::size_t limit) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) throw std::runtime_error("cannot open " + path.string());
    const auto end = input.tellg();
    if (end < 0 || static_cast<std::uintmax_t>(end) > limit) {
        throw std::runtime_error("input exceeds size limit or is not seekable: " + path.string());
    }
    std::vector<std::byte> bytes(static_cast<std::size_t>(end));
    input.seekg(0);
    if (!input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) {
        throw std::runtime_error("failed to read " + path.string());
    }
    if (input.peek() != std::char_traits<char>::eof()) throw std::runtime_error("input grew during read");
    return bytes;
}

std::string read_text(const std::filesystem::path& path, std::size_t limit) {
    const auto bytes = read_file(path, limit);
    if (bytes.empty()) return {};
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

void atomic_write(const std::filesystem::path& path, std::span<const std::byte> bytes) {
    auto temporary = path.string() + ".tmp.XXXXXX";
    const int descriptor = mkstemp(temporary.data());
    if (descriptor < 0) throw std::system_error(errno, std::generic_category(), "create temporary output");
    int fd = descriptor;
    try {
        while (!bytes.empty()) {
            const auto count = std::min<std::size_t>(bytes.size(), 1U << 20);
            const auto written = ::write(fd, bytes.data(), count);
            if (written < 0 && errno == EINTR) continue;
            if (written <= 0) throw std::system_error(written == 0 ? EIO : errno, std::generic_category(), "write output");
            bytes = bytes.subspan(static_cast<std::size_t>(written));
        }
        if (fsync(fd) < 0) throw std::system_error(errno, std::generic_category(), "sync output");
        fd = -1;
        if (close(descriptor) < 0) throw std::system_error(errno, std::generic_category(), "close output");
        if (rename(temporary.c_str(), path.c_str()) < 0) {
            throw std::system_error(errno, std::generic_category(), "commit output");
        }
    } catch (...) {
        if (fd >= 0) (void)close(fd);
        (void)unlink(temporary.c_str());
        throw;
    }
}

void atomic_write(const std::filesystem::path& path, std::string_view text) {
    atomic_write(path, std::as_bytes(std::span(text)));
}
}
