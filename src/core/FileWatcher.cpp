#include "core/FileWatcher.hpp"

namespace saida {

void WatchedFile::clear() {
    path_.clear();
    hasWriteTime_ = false;
    writeTime_ = {};
}

bool WatchedFile::watch(const std::string& path) {
    path_ = path;
    hasWriteTime_ = readWriteTime(path_, writeTime_);
    return hasWriteTime_;
}

bool WatchedFile::pollChanged() {
    if (path_.empty()) return false;

    std::filesystem::file_time_type current{};
    if (!readWriteTime(path_, current)) return false;

    if (!hasWriteTime_) {
        writeTime_ = current;
        hasWriteTime_ = true;
        return false;
    }

    if (current == writeTime_) return false;

    writeTime_ = current;
    return true;
}

bool WatchedFile::readWriteTime(const std::string& path, std::filesystem::file_time_type& outTime) {
    std::error_code ec;
    outTime = std::filesystem::last_write_time(path, ec);
    return !ec;
}

} // namespace saida
