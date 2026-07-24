// SaidaEngine standalone game runtime.
//
// This is the "export template": a pre-built, editor-less player executable that
// the editor's Build step copies next to the packaged game data. It links only
// saida_engine (no saida_editor / ImGui), resolves all assets relative to its own
// directory (Paths runtime root), reads a tiny boot manifest (game.saida) telling
// it which project + scene to launch, then runs the same engine loop a desktop
// game uses — mirroring the standalone path already used for XR preview in
// src/main.cpp.

#include "Engine.hpp"
#include "core/CrashReporter.hpp"
#include "core/Log.hpp"
#include "core/Paths.hpp"
#include "core/PlatformCaps.hpp"
#include "core/Time.hpp"
#include "runtime/BootManifest.hpp"
#include "runtime/TestAutoload.hpp"
#include "scene/SceneSerializer.hpp"

#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

namespace fs = std::filesystem;

// Directory containing this executable (where the packaged game data lives).
fs::path executableDir() {
#ifdef _WIN32
    wchar_t buf[MAX_PATH];
    DWORD n = ::GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n != 0 && n < MAX_PATH)
        return fs::path(std::wstring(buf, buf + n)).parent_path();
#endif
    return fs::current_path();
}

} // namespace

int main(int argc, char** argv) {
    saida::crash::install("SaidaEngineRuntime");
    try {
        std::vector<std::string> testAutoloads;
        for (int i = 1; i < argc; ++i) {
            if (std::string(argv[i]) == "--test-autoload" && i + 1 < argc)
                testAutoloads.emplace_back(argv[++i]);
        }

        const fs::path root = executableDir();

        // Every asset/shader lookup now resolves under the exe directory.
        saida::setRuntimeRoot(root.string());

        // Desktop player: everything is available except touch.
        saida::platform::setCapabilities(saida::platform::kAllCapabilities &
                                         ~uint32_t(saida::platform::Capability::TouchInput));
        saida::Log::info(saida::platform::report());

        // Boot manifest: key=value text written by the editor's Build step.
        const auto boot = saida::loadBootManifest((root / "game.saida").string());
        if (!boot.ok) throw std::runtime_error(boot.error);

        const std::string projectAbs =
            (root / boot.manifest.project).lexically_normal().string();
        const std::string sceneAbs =
            (root / boot.manifest.mainScene).lexically_normal().string();

        // Same standalone path as the XR preview: load project + scene, mount the
        // persistent World (autoloads), run unscaled (Play).
        saida::Engine engine(nullptr, projectAbs, false);

        // Shipped game: persist saves/prefs under the per-user OS data directory
        // (never next to the read-only exe), keyed by the game's identity. Set
        // before any autoload/script can touch storage. Falls back to the project
        // file stem if the project carries no name.
        std::string saveId = engine.project().name();
        if (saveId.empty()) saveId = fs::path(boot.manifest.project).stem().string();
        saida::setSaveIdentity(saveId);
        for (const std::string& spec : testAutoloads) {
            std::string error;
            if (!saida::runtime::applyTestAutoload(engine.project(), spec, error))
                throw std::runtime_error(error);
        }
        if (!saida::SceneSerializer::loadIntoScene(
                engine.scene(), engine.resources(), sceneAbs))
            throw std::runtime_error("failed to load main scene: " + sceneAbs);
        engine.mountWorld();
        saida::Time::setScale(1.0f);
        engine.run();
    } catch (const std::exception& e) {
        const auto report =
            saida::crash::writeFatalReport(std::string("fatal exception: ") + e.what());
        saida::Log::error(e.what());
        if (!report.logPath.empty())
            saida::Log::error("crash report: ", report.logPath.string());
        return EXIT_FAILURE;
    } catch (...) {
        const auto report =
            saida::crash::writeFatalReport("fatal non-standard exception");
        saida::Log::error("fatal non-standard exception");
        if (!report.logPath.empty())
            saida::Log::error("crash report: ", report.logPath.string());
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
