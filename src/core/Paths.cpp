#include "core/Paths.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <system_error>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#elif defined(__linux__)
#include <unistd.h>
#endif

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

std::string executableDirectory() {
#ifdef _WIN32
    std::wstring buffer(32768, L'\0');
    const DWORD length = ::GetModuleFileNameW(
        nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length != 0 && length < buffer.size()) {
        buffer.resize(length);
        return normalizeSeparators(fs::path(buffer).parent_path().string());
    }
#elif defined(__linux__)
    std::vector<char> buffer(4096, '\0');
    while (buffer.size() <= 1024 * 1024) {
        const ssize_t length = ::readlink("/proc/self/exe", buffer.data(),
                                          buffer.size() - 1);
        if (length < 0) break;
        if (static_cast<size_t>(length) < buffer.size() - 1) {
            buffer[static_cast<size_t>(length)] = '\0';
            return normalizeSeparators(
                fs::path(buffer.data()).parent_path().string());
        }
        buffer.resize(buffer.size() * 2, '\0');
    }
#endif
    std::error_code error;
    return normalizeSeparators(fs::current_path(error).string());
}

bool initializeInstalledLayout() {
    if (const char* overrideRoot = std::getenv("SAIDA_RUNTIME_ROOT")) {
        if (*overrideRoot != '\0') {
            setRuntimeRoot(normalizeSeparators(overrideRoot));
            return true;
        }
    }
    const fs::path root = executableDirectory();
    std::error_code error;
    if (fs::is_regular_file(root / "saida-install.json", error)) {
        setRuntimeRoot(normalizeSeparators(root.string()));
        return true;
    }
    return false;
}

bool installedLayout() { return !g_runtimeRoot.empty(); }

std::string engineRoot() {
    if (!g_runtimeRoot.empty()) return g_runtimeRoot;
    return normalizeSeparators(SAIDA_PROJECT_ROOT);
}

std::string shaderRoot() {
    if (!g_runtimeRoot.empty()) return g_runtimeRoot + "/shaders";
    return normalizeSeparators(SAIDA_SHADER_DIR);
}

std::string runtimeBinaryRoot() {
    if (!g_runtimeRoot.empty()) return g_runtimeRoot;
    return normalizeSeparators(SAIDA_RUNTIME_DIR);
}

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

std::string homeDirectory() {
#if defined(_WIN32)
    if (const char* profile = envOrNull("USERPROFILE"))
        return normalizeSeparators(profile);
#else
    if (const char* home = envOrNull("HOME")) return normalizeSeparators(home);
#endif
    return {};
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

std::string applicationStateRoot() {
    if (const char* value = envOrNull("SAIDA_STATE_DIR"))
        return stripTrailingSlash(normalizeSeparators(value));
    const std::string base = osUserDataBase();
    if (!base.empty()) return stripTrailingSlash(base) + "/SaidaEngine";
    return engineRoot() + "/.saida-state";
}

std::string applicationStatePath(const std::string& relative) {
    if (relative.empty()) return applicationStateRoot();
    return applicationStateRoot() + "/" + normalizeSeparators(relative);
}

std::string defaultProjectsRoot() {
    if (const char* value = envOrNull("SAIDA_PROJECTS_DIR"))
        return stripTrailingSlash(normalizeSeparators(value));
    const std::string home = homeDirectory();
    if (!home.empty()) return home + "/Documents/SaidaEngine/Projects";
    return applicationStateRoot() + "/Projects";
}

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
