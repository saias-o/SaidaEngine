#pragma once

#include <algorithm>
#include <filesystem>
#include <string>

// Centralized asset path resolution.
//
// Editor/dev mode (default): both roots are absolute paths baked at configure
// time by CMake, so the executable can run from any working directory.
//   SAIDA_PROJECT_ROOT : repository root (source assets: models/, assets/).
//   SAIDA_SHADER_DIR   : compiled SPIR-V output directory (build/shaders).
//
// Shipped-game mode: a packaged game has no access to those build paths. The
// runtime sets a "runtime root" (the directory of the game exe) at startup via
// setRuntimeRoot(); when set, assets resolve relative to it (<root>/<rel>) and
// shaders under <root>/shaders/. The public API below is unchanged, so every
// existing call site benefits from the redirection transparently.
namespace saida {

// Process/application layout.
//
// Development builds keep using the configure-time checkout/build roots. A
// published editor carries `saida-install.json` beside its executables; each
// entry point calls initializeInstalledLayout() before constructing engine
// services, which redirects every packaged resource lookup to that directory.
// SAIDA_RUNTIME_ROOT is an explicit override for verification and advanced use.
std::string executableDirectory();
bool initializeInstalledLayout();
bool installedLayout();
std::string engineRoot();
std::string shaderRoot();
std::string runtimeBinaryRoot();

// Per-user writable locations used by both development and installed builds.
// SAIDA_STATE_DIR / SAIDA_PROJECTS_DIR are deterministic CI and operator
// overrides. The default Windows roots are %APPDATA%/SaidaEngine and
// %USERPROFILE%/Documents/SaidaEngine/Projects.
std::string applicationStateRoot();
std::string applicationStatePath(const std::string& relative);
std::string defaultProjectsRoot();

// Set/clear the runtime root (directory of the shipped game exe). Empty string
// (the default) means dev/editor mode → baked absolute paths are used.
void setRuntimeRoot(const std::string& dir);
const std::string& runtimeRoot();  // empty when unset

// Save-location policy (shipped games).
//
// A shipped game must not write its saves next to its executable: Program Files
// is read-only and a portable copy is shared. Persistent saves/prefs therefore
// go to the per-user data directory of the OS, keyed by the game's identity. The
// editor/dev process leaves the identity empty, so saves stay under the project
// root where the developer sees them; the web player persists via IDBFS mounted
// at the project root by the shell and is unaffected.
//
// setSaveIdentity() is called once at boot by the packaged runtime with the
// game's name (sanitized to a safe directory component). userSaveRoot() returns
// the base directory the storage layer should own for saves/ and prefs/, or an
// empty string when the caller must fall back to the project root. Precedence:
//   1) $SAIDA_SAVE_DIR  — explicit override (CI/tests/power users, portable saves)
//   2) per-OS user data dir keyed by saveIdentity() when set (shipped game)
//   3) empty            — editor/dev/web → project root
void setSaveIdentity(const std::string& appName);
const std::string& saveIdentity();  // sanitized; empty in editor/dev
std::string userSaveRoot();         // empty → caller uses the project root

// Root of the currently loaded .saidaproj (editor or runtime). Used to resolve
// project content files (scripts, ui) that aren't in the asset registry.
void setActiveProjectRoot(const std::string& dir);
const std::string& activeProjectRoot();  // empty when no project is loaded

struct SandboxedPathResult {
    bool ok = false;
    std::string absolute;
    std::string relative;
    std::string error;

    explicit operator bool() const { return ok; }
};

// Resolve a user-supplied project-relative path under projectRoot. This is the
// shared guard for tools/MCP/web workers: callers may pass "foo.js" and a
// defaultDirectory such as "scripts", but absolute paths, drive-qualified paths,
// parent traversal and symlink escapes are rejected before touching the target.
SandboxedPathResult resolveSandboxedProjectPath(const std::string& projectRoot,
                                                const std::string& userPath,
                                                const std::string& defaultDirectory = {});

inline std::string assetPath(const std::string& relative) {
#ifdef __EMSCRIPTEN__
    return "/assets/" + relative;
#else
    return engineRoot() + "/" + relative;
#endif
}

// Resolve project *content* that lives outside the asset registry: UI documents
// (.html/.rml), their stylesheets and scripts. Unlike assetPath(), which points
// at the engine root in editor/dev mode, this prefers the loaded .saidaproj, so
// "ui/main_menu.html" in a game project resolves under that project. Falls back
// to assetPath() (engine root / shipped runtime root) for built-in content.
inline std::string projectContentPath(const std::string& relative) {
    const std::string& project = activeProjectRoot();
    if (!project.empty()) {
        std::filesystem::path candidate = std::filesystem::path(project) / relative;
        std::error_code ec;
        if (std::filesystem::exists(candidate, ec)) return candidate.generic_string();
    }
    return assetPath(relative);
}

inline std::string shaderPath(const std::string& name) {
#ifdef __EMSCRIPTEN__
    // Web mode: the transpiled WGSL lives in MEMFS under /shaders. The web
    // backend swaps the extension (.spv -> .wgsl) so desktop call sites that
    // name SPIR-V files keep working unchanged.
    std::string webName = name;
    const std::string spv = ".spv";
    if (webName.size() > spv.size() &&
        webName.compare(webName.size() - spv.size(), spv.size(), spv) == 0) {
        webName = webName.substr(0, webName.size() - spv.size()) + ".wgsl";
    }
    return "/shaders/" + webName;
#else
    return shaderRoot() + "/" + name;
#endif
}

inline std::string pathFromFileUrl(const std::string& pathOrUrl) {
    constexpr const char* kFilePrefix = "file://";
    if (pathOrUrl.rfind(kFilePrefix, 0) != 0) return pathOrUrl;

    std::string path = pathOrUrl.substr(std::char_traits<char>::length(kFilePrefix));
    while (!path.empty() && path.front() == '/' && path.size() > 2 && path[2] == ':') {
        path.erase(path.begin());
    }
    std::replace(path.begin(), path.end(), '\\', '/');
    return path;
}

} // namespace saida
