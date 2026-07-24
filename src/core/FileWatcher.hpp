#pragma once

#include <filesystem>
#include <string>

namespace saida {

class WatchedFile {
public:
    void clear();
    bool watch(const std::string& path);
    bool pollChanged();

    const std::string& path() const { return path_; }
    bool active() const { return !path_.empty(); }

private:
    static bool readWriteTime(const std::string& path, std::filesystem::file_time_type& outTime);

    std::string path_;
    std::filesystem::file_time_type writeTime_{};
    bool hasWriteTime_ = false;
};

} // namespace saida
