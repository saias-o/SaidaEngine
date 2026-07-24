#pragma once

#include <string>

namespace saida {

// Result of a project rename. On failure `error` names the refusing step and
// the disk is left exactly as it was before the call.
struct ProjectRenameResult {
    bool ok = false;
    std::string error;
    std::string rootPath;         // new project directory
    std::string projectFilePath;  // new .saidaproj path

    explicit operator bool() const { return ok; }
};

// A project name must stay a safe single path component: no separators or
// traversal, no characters Windows forbids in file names, no leading or
// trailing dot or space, at most 128 characters.
bool isValidProjectName(const std::string& name, std::string& error);

// Rename a project so its directory, its `.saidaproj` (file name and `name`
// field) and the Hub registry stay coherent. The steps run in an order whose
// intermediate states all still load, and any failure rolls every completed
// step back. `hubJsonPath` may be empty or point to a missing file, in which
// case no Hub entry is touched; a present but unreadable registry refuses the
// rename instead of leaving a stale entry behind.
ProjectRenameResult renameProjectDirectory(const std::string& rootPath,
                                           const std::string& newName,
                                           const std::string& hubJsonPath);

} // namespace saida
