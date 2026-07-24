#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace saida {

struct AtomicWriteResult {
    bool ok = false;
    std::string error;

    explicit operator bool() const { return ok; }
};

AtomicWriteResult writeFileAtomically(const std::filesystem::path& destination,
                                      std::string_view contents);

} // namespace saida
