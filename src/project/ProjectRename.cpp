#include "project/ProjectRename.hpp"

#include "core/AtomicFile.hpp"
#include "core/FormatVersions.hpp"
#include "core/Log.hpp"
#include "project/Project.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

namespace saida {
namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;

constexpr std::size_t kMaxProjectNameLength = 128;
constexpr const char* kProjectExtension = ".saidaproj";

bool readFileBytes(const fs::path& path, std::string& out, std::string& error) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        error = "cannot open " + path.string();
        return false;
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    out = buffer.str();
    return true;
}

// hub.json stores paths as the strings the Hub wrote; compare their normalized
// forms so slash direction and trailing separators do not matter.
bool samePathText(const std::string& a, const std::string& b) {
    return fs::path(a).lexically_normal() == fs::path(b).lexically_normal();
}

} // namespace

bool isValidProjectName(const std::string& name, std::string& error) {
    if (name.empty()) {
        error = "project name is empty";
        return false;
    }
    if (name.size() > kMaxProjectNameLength) {
        error = "project name exceeds 128 characters";
        return false;
    }
    if (name.front() == ' ' || name.back() == ' ') {
        error = "project name may not start or end with a space";
        return false;
    }
    // Also rejects "." and "..", so a name can never traverse directories.
    if (name.front() == '.' || name.back() == '.') {
        error = "project name may not start or end with '.'";
        return false;
    }
    for (const char c : name) {
        const bool forbidden = c == '/' || c == '\\' || c == ':' || c == '*' ||
                               c == '?' || c == '"' || c == '<' || c == '>' ||
                               c == '|' || static_cast<unsigned char>(c) < 0x20;
        if (forbidden) {
            error = "project name contains a forbidden character";
            return false;
        }
    }
    return true;
}

ProjectRenameResult renameProjectDirectory(const std::string& rootPath,
                                           const std::string& newName,
                                           const std::string& hubJsonPath) {
    ProjectRenameResult failure;
    if (!isValidProjectName(newName, failure.error)) return failure;

    std::error_code ec;
    const fs::path oldRoot(rootPath);
    if (!fs::is_directory(oldRoot, ec)) {
        failure.error = "project directory not found: " + rootPath;
        return failure;
    }

    const std::string oldProjectFile =
        resolveProjectFileInDirectory(rootPath, failure.error);
    if (oldProjectFile.empty()) return failure;

    const fs::path oldProjPath(oldProjectFile);
    const fs::path renamedProjPath = oldRoot / (newName + kProjectExtension);
    const fs::path newRoot = oldRoot.parent_path() / newName;
    const bool projFileNeedsRename =
        oldProjPath.filename() != renamedProjPath.filename();
    const bool dirNeedsRename = oldRoot.filename() != newRoot.filename();

    if (dirNeedsRename && fs::exists(newRoot, ec) &&
        !fs::equivalent(newRoot, oldRoot, ec)) {
        failure.error = "target directory already exists: " + newRoot.string();
        return failure;
    }
    if (projFileNeedsRename && fs::exists(renamedProjPath, ec)) {
        failure.error =
            "target project file already exists: " + renamedProjPath.string();
        return failure;
    }

    // The project file must be a current JSON document.
    std::string originalBytes;
    if (!readFileBytes(oldProjPath, originalBytes, failure.error)) return failure;
    json doc = json::parse(originalBytes, nullptr, /*allow_exceptions=*/false);
    if (doc.is_discarded() || !doc.is_object()) {
        failure.error = "project file is not a JSON document: " + oldProjPath.string();
        return failure;
    }
    if (const std::string envelope =
            format::schemaEnvelopeError(doc, format::kProjectVersion, "project");
        !envelope.empty()) {
        failure.error = envelope;
        return failure;
    }
    doc["name"] = newName;

    // Load the Hub registry before mutating anything: a corrupt registry
    // refuses the rename instead of leaving a stale entry behind.
    json hubDoc;
    bool updateHub = false;
    if (!hubJsonPath.empty() && fs::exists(hubJsonPath, ec)) {
        std::string hubBytes;
        if (!readFileBytes(hubJsonPath, hubBytes, failure.error)) return failure;
        hubDoc = json::parse(hubBytes, nullptr, /*allow_exceptions=*/false);
        if (hubDoc.is_discarded() || !hubDoc.is_object()) {
            failure.error = "hub registry is not valid JSON: " + hubJsonPath;
            return failure;
        }
        if (auto it = hubDoc.find("projects"); it != hubDoc.end() && it->is_array()) {
            for (auto& entry : *it) {
                if (!entry.is_object()) continue;
                const std::string entryPath = entry.value("path", "");
                if (samePathText(entryPath, rootPath) ||
                    samePathText(entryPath, oldProjectFile)) {
                    entry["name"] = newName;
                    entry["path"] = newRoot.string();
                    updateHub = true;
                }
            }
        }
    }

    // Step 1 — rename the .saidaproj inside its directory (atomic). Every
    // intermediate state from here on still loads: a name-field/file-name
    // mismatch is tolerated by Project::load.
    if (projFileNeedsRename) {
        fs::rename(oldProjPath, renamedProjPath, ec);
        if (ec) {
            failure.error = "cannot rename project file: " + ec.message();
            return failure;
        }
    }
    const auto rollbackProjectFile = [&]() {
        const AtomicWriteResult restored =
            writeFileAtomically(renamedProjPath, originalBytes);
        std::error_code rb;
        if (projFileNeedsRename) fs::rename(renamedProjPath, oldProjPath, rb);
        if (!restored || rb)
            Log::error("project rename rollback failed for ",
                       oldProjPath.string(), "; repair the file name manually");
    };

    // Step 2 — rewrite the name field (atomic replace, same format as save()).
    const AtomicWriteResult wrote =
        writeFileAtomically(renamedProjPath, doc.dump(2) + "\n");
    if (!wrote) {
        rollbackProjectFile();
        failure.error = "cannot rewrite project file: " + wrote.error;
        return failure;
    }

    // Step 3 — rename the directory (atomic).
    if (dirNeedsRename) {
        fs::rename(oldRoot, newRoot, ec);
        if (ec) {
            rollbackProjectFile();
            failure.error = "cannot rename project directory: " + ec.message();
            return failure;
        }
    }

    // Step 4 — update the Hub registry (atomic replace).
    if (updateHub) {
        const AtomicWriteResult hubWrote =
            writeFileAtomically(hubJsonPath, hubDoc.dump(4));
        if (!hubWrote) {
            std::error_code rb;
            if (dirNeedsRename) fs::rename(newRoot, oldRoot, rb);
            if (rb) {
                Log::error("project rename rollback failed for ",
                           oldRoot.string(), "; repair the directory manually");
            } else {
                rollbackProjectFile();
            }
            failure.error = "cannot update hub registry: " + hubWrote.error;
            return failure;
        }
    }

    ProjectRenameResult ok;
    ok.ok = true;
    ok.rootPath = newRoot.string();
    ok.projectFilePath = (newRoot / renamedProjPath.filename()).string();
    return ok;
}

} // namespace saida
