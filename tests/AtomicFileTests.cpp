#include "core/AtomicFile.hpp"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace {

std::string readFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    std::ostringstream contents;
    contents << file.rdbuf();
    return contents.str();
}

std::size_t temporaryFileCount(const std::filesystem::path& directory) {
    std::size_t count = 0;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (entry.path().filename().string().find(".saida-tmp-") != std::string::npos)
            ++count;
    }
    return count;
}

} // namespace

int main() {
    namespace fs = std::filesystem;

    const fs::path sandbox = fs::temp_directory_path() / "saida-atomic-file-tests";
    std::error_code error;
    fs::remove_all(sandbox, error);
    fs::create_directories(sandbox);

    const fs::path save = sandbox / "save.json";
    {
        std::ofstream initial(save, std::ios::binary);
        initial << "old";
    }

    assert(saida::writeFileAtomically(save, "new"));
    assert(readFile(save) == "new");
    assert(temporaryFileCount(sandbox) == 0);

    assert(saida::writeFileAtomically(save, ""));
    assert(readFile(save).empty());

    const fs::path directoryTarget = sandbox / "directory-target";
    fs::create_directory(directoryTarget);
    assert(!saida::writeFileAtomically(directoryTarget, "rejected"));
    assert(fs::is_directory(directoryTarget));
    assert(temporaryFileCount(sandbox) == 0);

    fs::remove_all(sandbox, error);
    return 0;
}
