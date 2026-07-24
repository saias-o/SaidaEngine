#include "core/Paths.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <utility>

namespace saida {

namespace {
namespace fs = std::filesystem;

std::string normalizeSeparators(std::string path) {
    std::replace(path.begin(), path.end(), '\\', '/');
    return path;
}

std::string lowerForCompare(std::string value) {
#ifdef _WIN32
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
#endif
    return value;
}

bool isRootedOrDriveQualified(const fs::path& path, const std::string& raw) {
    if (path.is_absolute() || path.has_root_name() || path.has_root_directory()) return true;
    return raw.find(':') != std::string::npos;
}

bool containsParentTraversal(const fs::path& path) {
    for (const auto& part : path) {
        if (part == "..") return true;
    }
    return false;
}

std::string stripTrailingSlash(std::string path) {
    while (path.size() > 1 && path.back() == '/') path.pop_back();
    return path;
}

bool isInsideRoot(const fs::path& root, const fs::path& child) {
    const std::string rootText = lowerForCompare(stripTrailingSlash(root.generic_string()));
    const std::string childText = lowerForCompare(stripTrailingSlash(child.generic_string()));
    return childText == rootText ||
           (childText.size() > rootText.size() &&
            childText.compare(0, rootText.size(), rootText) == 0 &&
            childText[rootText.size()] == '/');
}

SandboxedPathResult reject(std::string error) {
    SandboxedPathResult result;
    result.error = std::move(error);
    return result;
}
// Directory of the shipped game executable, set once at runtime startup.
// Empty in the editor/dev process → baked absolute paths are used instead.
std::string g_runtimeRoot;
} // namespace

void setRuntimeRoot(const std::string& dir) { g_runtimeRoot = dir; }

const std::string& runtimeRoot() { return g_runtimeRoot; }

namespace {
// Root of the currently loaded .saidaproj project (editor or runtime).
// Resolves project content files (scripts, ui) outside the asset
// registry. Empty when no project is loaded.
std::string g_activeProjectRoot;
} // namespace

void setActiveProjectRoot(const std::string& dir) { g_activeProjectRoot = dir; }

const std::string& activeProjectRoot() { return g_activeProjectRoot; }

namespace {
// Identity of the packaged game, keying its user save folder (sanitized name).
// Empty in the editor/dev — saves stay under the project root.
std::string g_saveIdentity;

const char* envOrNull(const char* name) {
    const char* v = std::getenv(name);
    return (v && *v) ? v : nullptr;
}

// Reduces a game name to a safe folder component: [A-Za-z0-9._-], spaces
// become '_', everything else is dropped; no leading '.'/'_' (POSIX hidden
// folder and '..'), no trailing '.'/space, length-capped. Empty if nothing
// usable remains.
std::string sanitizeIdentity(const std::string& name) {
    std::string out;
    out.reserve(name.size());
    for (unsigned char c : name) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.')
            out.push_back(static_cast<char>(c));
        else if (c == ' ')
            out.push_back('_');
    }
    const std::size_t start = out.find_first_not_of("._");
    if (start == std::string::npos) return {};
    out.erase(0, start);
    while (!out.empty() && (out.back() == '.' || out.back() == ' ')) out.pop_back();
    if (out.size() > 64) out.resize(64);
    return out;
}

// Base of the OS's user data folder, or empty if unresolvable.
std::string osUserDataBase() {
#if defined(_WIN32)
    if (const char* p = envOrNull("APPDATA")) return normalizeSeparators(p);
    if (const char* p = envOrNull("LOCALAPPDATA")) return normalizeSeparators(p);
    return {};
#elif defined(__APPLE__)
    if (const char* home = envOrNull("HOME"))
        return normalizeSeparators(home) + "/Library/Application Support";
    return {};
#else
    if (const char* p = envOrNull("XDG_DATA_HOME")) return normalizeSeparators(p);
    if (const char* home = envOrNull("HOME")) return normalizeSeparators(home) + "/.local/share";
    return {};
#endif
}
} // namespace

void setSaveIdentity(const std::string& appName) {
    g_saveIdentity = sanitizeIdentity(appName);
}

const std::string& saveIdentity() { return g_saveIdentity; }

std::string userSaveRoot() {
#ifdef __EMSCRIPTEN__
    // The web player persists via IDBFS mounted at the project root by the shell.
    return {};
#else
    if (const char* env = envOrNull("SAIDA_SAVE_DIR"))
        return stripTrailingSlash(normalizeSeparators(env));
    if (g_saveIdentity.empty()) return {};  // editor/dev -> project root
    const std::string base = osUserDataBase();
    if (base.empty()) return {};            // no OS location -> fall back to project root
    return stripTrailingSlash(base) + "/SaidaEngine/Games/" + g_saveIdentity;
#endif
}

SandboxedPathResult resolveSandboxedProjectPath(const std::string& projectRoot,
                                                const std::string& userPath,
                                                const std::string& defaultDirectory) {
    if (projectRoot.empty()) return reject("project root is required");

    std::string raw = normalizeSeparators(pathFromFileUrl(userPath));
    if (raw.empty()) return reject("path is required");

    fs::path relative(raw);
    if (isRootedOrDriveQualified(relative, raw))
        return reject("path must be project-relative");

    if (!defaultDirectory.empty() && !relative.has_parent_path()) {
        std::string defaultRaw = normalizeSeparators(defaultDirectory);
        fs::path defaultPath(defaultRaw);
        if (isRootedOrDriveQualified(defaultPath, defaultRaw))
            return reject("default directory must be project-relative");
        relative = defaultPath / relative;
    }

    relative = relative.lexically_normal();
    if (relative.empty() || relative == ".")
        return reject("path must name a project file");
    if (containsParentTraversal(relative))
        return reject("path must not contain parent traversal");

    std::error_code ec;
    const fs::path rootAbsolute =
        fs::absolute(fs::path(projectRoot), ec).lexically_normal();
    if (ec) return reject("could not resolve project root");
    const fs::path root = fs::weakly_canonical(rootAbsolute, ec);
    if (ec) return reject("could not canonicalize project root");

    // weakly_canonical resolves symlinks in the existing prefix while keeping
    // a not-yet-created leaf usable for authoring writes.
    const fs::path absolute = fs::weakly_canonical(root / relative, ec);
    if (ec) return reject("could not canonicalize project path");
    if (!isInsideRoot(root, absolute))
        return reject("path escapes project root");

    SandboxedPathResult result;
    result.ok = true;
    result.absolute = absolute.generic_string();
    result.relative = relative.generic_string();
    return result;
}

} // namespace saida
