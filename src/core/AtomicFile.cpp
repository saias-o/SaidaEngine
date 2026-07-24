#include "core/AtomicFile.hpp"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <system_error>

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#elif !defined(__EMSCRIPTEN__)
#include <unistd.h>
#endif

namespace saida {
namespace {

constexpr const char* kTemporaryMarker = ".saida-tmp-";

std::filesystem::path temporaryPath(const std::filesystem::path& destination) {
    static std::atomic<std::uint64_t> sequence{0};
    const auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
    std::filesystem::path path = destination;
    path += kTemporaryMarker + std::to_string(timestamp) + "-" +
            std::to_string(sequence.fetch_add(1, std::memory_order_relaxed));
    return path;
}

std::FILE* openTemporaryFile(const std::filesystem::path& path) {
#ifdef _WIN32
    return _wfopen(path.c_str(), L"wb");
#else
    return std::fopen(path.c_str(), "wb");
#endif
}

bool flushFile(std::FILE* file) {
    if (std::fflush(file) != 0) return false;
#ifdef _WIN32
    return _commit(_fileno(file)) == 0;
#elif defined(__EMSCRIPTEN__)
    return true;
#else
    return ::fsync(fileno(file)) == 0;
#endif
}

AtomicWriteResult replaceFile(const std::filesystem::path& temporary,
                              const std::filesystem::path& destination) {
#ifdef _WIN32
    constexpr DWORD kReplaceFlags = MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH;
    if (::MoveFileExW(temporary.c_str(), destination.c_str(), kReplaceFlags))
        return {true, {}};
    return {false, std::error_code(static_cast<int>(::GetLastError()),
                                   std::system_category()).message()};
#else
    std::error_code error;
    std::filesystem::rename(temporary, destination, error);
    if (error) return {false, error.message()};
    return {true, {}};
#endif
}

} // namespace

AtomicWriteResult writeFileAtomically(const std::filesystem::path& destination,
                                      std::string_view contents) {
    if (destination.empty()) return {false, "destination path is empty"};

    const std::filesystem::path temporary = temporaryPath(destination);
    std::FILE* file = openTemporaryFile(temporary);
    if (!file) return {false, std::strerror(errno)};

    const bool written = contents.empty() ||
        std::fwrite(contents.data(), 1, contents.size(), file) == contents.size();
    const bool flushed = written && flushFile(file);
    const bool closed = std::fclose(file) == 0;
    const int writeError = errno == 0 ? EIO : errno;
    if (!written || !flushed || !closed) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return {false, std::strerror(writeError)};
    }

    AtomicWriteResult result = replaceFile(temporary, destination);
    if (!result) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
    }
    return result;
}

} // namespace saida
