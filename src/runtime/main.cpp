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
#include "runtime/CaptureArgs.hpp"
#include "runtime/ProfileArgs.hpp"
#include "runtime/ProjectArgs.hpp"
#include "runtime/TestAutoload.hpp"
#include "scene/SceneSerializer.hpp"

#ifdef SAIDA_GAME_TYPES_HEADER
#include SAIDA_GAME_TYPES_HEADER
#endif

#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

} // namespace

int main(int argc, char** argv) {
    saida::crash::install("SaidaEngineRuntime");
    try {
        std::vector<std::string> testAutoloads;
        for (int i = 1; i < argc; ++i) {
            if (std::string(argv[i]) == "--test-autoload" && i + 1 < argc)
                testAutoloads.emplace_back(argv[++i]);
        }

        // The player draws the game and nothing else — no editor chrome, no
        // live FPS or camera overlay — so its capture is the one a golden image
        // is taken from. The editor's differs between two runs of the same
        // command by exactly those overlays.
        saida::CaptureRequest capture;
        saida::runtime::CaptureViewpoint viewpoint;
        std::string captureError;
        if (!saida::runtime::parseCaptureArgs(argc, argv, capture, viewpoint,
                                              captureError)) {
            // A mistyped flag is a usage error, not a crash: writing a crash
            // report for it buries real reports under typos and tells the caller
            // to file a bug against the engine instead of fixing the command.
            saida::Log::error(captureError);
            return EXIT_FAILURE;
        }
        std::string profilePath;
        if (!saida::runtime::parseProfileArgs(argc, argv, profilePath, captureError)) {
            saida::Log::error(captureError);
            return EXIT_FAILURE;
        }

        std::string projectAbs, sceneAbs;
        if (!saida::runtime::parseProjectArgs(argc, argv, projectAbs, captureError))
            throw std::runtime_error(captureError);
        const fs::path root = saida::executableDirectory();
#ifdef SAIDA_DEFAULT_PROJECT
        // A packaged boot manifest takes precedence over the development path.
        if (projectAbs.empty() && !fs::exists(root / "game.saida"))
            projectAbs = SAIDA_DEFAULT_PROJECT;
#endif
        const bool packaged = projectAbs.empty();
        if (packaged) {
            saida::setRuntimeRoot(root.string());
            const auto boot = saida::loadBootManifest((root / "game.saida").string());
            if (!boot.ok) throw std::runtime_error(boot.error);
            projectAbs = (root / boot.manifest.project).lexically_normal().string();
            sceneAbs = (root / boot.manifest.mainScene).lexically_normal().string();
        }

        // Desktop player: everything is available except touch.
        saida::platform::setCapabilities(saida::platform::kAllCapabilities &
                                         ~uint32_t(saida::platform::Capability::TouchInput));
        saida::Log::info(saida::platform::report());

        // Same standalone path as the XR preview: load project + scene, mount the
        // persistent World (autoloads), run unscaled (Play).
#ifdef SAIDA_GAME_TYPES_HEADER
        saida::registerProjectTypes();
#endif
        saida::Engine engine(nullptr, projectAbs, false);
        engine.profileTo(profilePath);
        if (!engine.project().isLoaded()) throw std::runtime_error("failed to load project: " + projectAbs);
        if (!packaged) {
            const auto main = saida::resolveSandboxedProjectPath(
                engine.project().rootPath(), engine.project().mainScene());
            if (engine.project().mainScene().empty() || !main)
                throw std::runtime_error("project needs a valid project-relative mainScene");
            sceneAbs = main.absolute;
        }

        // Shipped game: persist saves/prefs under the per-user OS data directory
        // (never next to the read-only exe), keyed by the game's identity. Set
        // before any autoload/script can touch storage. Falls back to the project
        // file stem if the project carries no name.
        std::string saveId = engine.project().name();
        if (saveId.empty()) saveId = fs::path(projectAbs).stem().string();
        if (packaged) saida::setSaveIdentity(saveId);
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
        if (viewpoint.set)
            engine.setCameraOverride({viewpoint.position[0], viewpoint.position[1],
                                      viewpoint.position[2]},
                                     {viewpoint.target[0], viewpoint.target[1],
                                      viewpoint.target[2]});
        engine.setCameraFovOverride(viewpoint.fovDegrees);
        // A rejected viewpoint must stop the run here. Entering the loop with no
        // capture armed would never close the window: the process would hang
        // instead of reporting, which is the one outcome worse than a bad image.
        if (engine.captureFailed()) return EXIT_FAILURE;

        if (!capture.pngPath.empty()) engine.captureFrameThenExit(capture);
        engine.run();
        if (engine.captureFailed()) return EXIT_FAILURE;
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
