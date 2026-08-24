#include "Engine.hpp"
#include "core/CrashReporter.hpp"
#include "core/Log.hpp"
#include "core/Paths.hpp"
#include "editor/EditorApp.hpp"
#include "core/Time.hpp"
#include "runtime/CaptureArgs.hpp"
#include "runtime/TestAutoload.hpp"
#include "runtime/RuntimeRoundTripContract.hpp"
#include "scene/SceneSerializer.hpp"

#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    saida::initializeInstalledLayout();
    saida::crash::install("SaidaEngine");
    try {
        std::string initialProject;
        std::string runtimeScene;
        std::string buildOutputDir;
        bool xrPreview = false;
        bool readXrPreviewManifest = false;
        bool buildRequested = false;
        bool buildWeb = false;
        bool playRequested = false;
        bool verifyRuntimeContract = false;
        std::vector<std::string> testAutoloads;

        saida::CaptureRequest capture;
        saida::runtime::CaptureViewpoint viewpoint;
        std::string captureError;
        if (!saida::runtime::parseCaptureArgs(argc, argv, capture, viewpoint,
                                              captureError)) {
            // A mistyped flag is a usage error, not a crash (see runtime/main.cpp).
            saida::Log::error(captureError);
            return EXIT_FAILURE;
        }

        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--project" && i + 1 < argc)
                initialProject = argv[++i];
            else if (arg == "--scene" && i + 1 < argc)
                runtimeScene = argv[++i];
            else if (arg == "--build" && i + 1 < argc) {
                buildRequested = true;
                buildOutputDir = argv[++i];
            } else if (arg == "--build-platform" && i + 1 < argc)
                buildWeb = (std::string(argv[++i]) == "web");
            else if (arg == "--play")
                playRequested = true;
            else if (arg == "--test-autoload" && i + 1 < argc)
                testAutoloads.emplace_back(argv[++i]);
            else if (arg == "--verify-runtime-contract")
                verifyRuntimeContract = true;
            else if (arg == "--xr")
                xrPreview = true;
            else if (arg == "--xr-preview") {
                xrPreview = true;
                readXrPreviewManifest = true;
            }
        }

        if (readXrPreviewManifest) {
            const std::filesystem::path manifestPath =
                saida::applicationStatePath("xr/xr_preview.launch");
            std::ifstream manifest(manifestPath);
            if (!manifest || !std::getline(manifest, initialProject) ||
                !std::getline(manifest, runtimeScene) || initialProject.empty() ||
                runtimeScene.empty())
                throw std::runtime_error(
                    "invalid XR preview launch manifest: " + manifestPath.string());
        }

        // XR Preview is a standalone child process launched by the desktop editor.
        // It must create its Vulkan device through OpenXR from process startup;
        // switching the editor's existing desktop device in place is not valid.
        //
        // No SceneSetup: launched directly (no --project), the editor opens on an
        // empty scene. The Hub passes a project via --project to load real content.
        saida::Engine engine(nullptr, initialProject, xrPreview);
        for (const std::string& spec : testAutoloads) {
            std::string error;
            if (!saida::runtime::applyTestAutoload(engine.project(), spec, error))
                throw std::runtime_error(error);
        }
        if (verifyRuntimeContract) {
            saida::runtime::RoundTripContractReport report;
            std::string error;
            if (!saida::runtime::verifyRuntimeRoundTripContract(
                    saida::RuntimeTypeTarget::Native, engine.resources(), report, error))
                throw std::runtime_error("native runtime contract: " + error);
            saida::Log::info("[CONTRACT] PASS native nodes=", report.nodes,
                             " behaviours=", report.behaviours,
                             " properties=", report.reflectedProperties);
            return EXIT_SUCCESS;
        }
        if (xrPreview) {
            if (runtimeScene.empty())
                throw std::runtime_error("--xr requires --scene <path>");
            const std::string scenePath =
                std::filesystem::absolute(runtimeScene).lexically_normal().string();
            if (!saida::SceneSerializer::loadIntoScene(
                    engine.scene(), engine.resources(), scenePath))
                throw std::runtime_error("failed to load XR preview scene: " + scenePath);
            engine.mountWorld();
            saida::Time::setScale(1.0f);
            engine.run();
            return EXIT_SUCCESS;
        }

        // Normal desktop editor.
        saida::EditorApp editor(engine);

        // --build <out>: automated Build click — same code as the Build dialog
        // button, then immediate exit with the verdict as the return code.
        if (buildRequested) return editor.runAutomatedBuild(buildWeb, buildOutputDir);

        // --play: opens the project then triggers the same Play transition as the
        // editor button. Useful for automated recipes with a game driver that
        // ends the process itself via tree.quit().
        if (playRequested) editor.setPlayMode(true);

        // --screenshot: wait for the world to load, draw a fixed number of
        // frames on a pinned clock, write the composite, then exit. The editor's
        // capture still includes its live overlays, so a golden image is taken
        // from SaidaEngineRuntime; this one is for looking at the editor itself.
        if (viewpoint.set)
            engine.setCameraOverride({viewpoint.position[0], viewpoint.position[1],
                                      viewpoint.position[2]},
                                     {viewpoint.target[0], viewpoint.target[1],
                                      viewpoint.target[2]});
        // A rejected viewpoint stops the run before the loop (see runtime/main.cpp).
        if (engine.captureFailed()) return EXIT_FAILURE;

        if (!capture.pngPath.empty()) engine.captureFrameThenExit(capture);

        engine.setOnFrame([&editor](float dt) { editor.update(dt); });
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
