// SaidaEngine web player, distinct from the authoring runtime.
//
// It boots exactly like the desktop player (src/runtime/main.cpp): reads the
// game.saida manifest, loads the project and main scene through the REAL
// SceneSerializer (behaviours, signals, groups), mounts the SceneTree's
// persistent World (autoloads), then runs the game loop — behaviour updates,
// CameraDirector, deferred operations, timers — and renders via the WebGPU backend.
//
// Physics, audio, and the RmlUi HUD are all active. UI types not yet ported
// stay rejected at load time by the fail-closed registry.

#include "audio/AudioManager.hpp"
#include "core/Camera.hpp"
#include "core/FormatVersions.hpp"
#include "core/Input.hpp"
#include "core/Log.hpp"
#include "core/PlatformCaps.hpp"
#include "core/Time.hpp"
#include "graphics/ResourceManager.hpp"
#include "project/AssetRegistry.hpp"
#include "project/Project.hpp"
#include "render/CameraDirector.hpp"
#include "render/RenderFeatureRegistry.hpp"
#include "render/Renderer.hpp"
#include "rhi/webgpu/RhiWeb.hpp"
#include "runtime/BootManifest.hpp"
#include "runtime/TestAutoload.hpp"
#include "runtime/RuntimeRoundTripContract.hpp"
#include "scene/BehaviourRegistry.hpp"
#include "nodes/CameraNode.hpp"
#include "nodes/MeshNode.hpp"
#include "scene/NodeRegistry.hpp"
#include "scene/RuntimeTypeMatrix.hpp"
#include "scene/ReflectedTypes.hpp"
#include "scene/Scene.hpp"
#include "scene/SceneSerializer.hpp"
#include "scene/SceneTree.hpp"
#include "nodes/UICanvasNode.hpp"
#include "nodes/UINode.hpp"
#include "nodes/UITextNode.hpp"
#include "scripting/JsRuntime.hpp"

#include <GLFW/glfw3.h>
#include <emscripten.h>
#include <nlohmann/json.hpp>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using namespace saida;

namespace {

constexpr uint32_t kWidth = 1280, kHeight = 720;

struct PlayerApp {
    std::unique_ptr<rhi::Device> device;
    std::unique_ptr<rhi::Surface> surface;
    std::unique_ptr<AssetRegistry> registry;
    std::unique_ptr<ResourceManager> resources;
    std::unique_ptr<Renderer> renderer;
    std::unique_ptr<SceneTree> tree;
    std::unique_ptr<Project> project;
    Scene* world = nullptr;  // owned by the SceneTree once mounted
    Camera camera;
    CameraDirector cameraDirector;
    double lastMs = 0.0;
    bool running = false;
    std::string state = "initializing";
    std::string startupError;
};

PlayerApp gApp;
std::vector<std::string> gTestAutoloads;
bool gVerifyRuntimeContract = false;

bool failBoot(const std::string& error) {
    gApp.running = false;
    gApp.state = "error";
    gApp.startupError = error;
    Log::error("saida-player: ", error);
    return false;
}

bool hasSuffix(const std::string& value, const char* suffix) {
    const std::size_t suffixLength = std::char_traits<char>::length(suffix);
    return value.size() > suffixLength &&
           value.compare(value.size() - suffixLength, suffixLength, suffix) == 0;
}

bool bootGame() {
    // The game package is mounted in MEMFS under /project (same contract as
    // the desktop exe's folder): game.saida, .saidaproj, scenes, and assets.
    gApp.registry = std::make_unique<AssetRegistry>();
    gApp.registry->load("/project");
    chdir("/project");

    const auto boot = loadBootManifest("/project/game.saida");
    if (!boot.ok) {
        return failBoot(boot.error);
    }

    gApp.project = std::make_unique<Project>();
    if (!gApp.project->load("/project/" + boot.manifest.project)) {
        return failBoot("cannot load project " + boot.manifest.project);
    }
    for (const std::string& spec : gTestAutoloads) {
        std::string error;
        if (!runtime::applyTestAutoload(*gApp.project, spec, error))
            return failBoot(error);
    }

    // Reflected types + base types. The registry is intentionally kept
    // explicit: an unported type fails the preflight check.
    registerReflectedTypes();
    NodeRegistry::instance().registerType<Node>("Node");
    NodeRegistry::instance().registerType<Scene>("Scene");
    NodeRegistry::instance().registerType<MeshNode>("MeshNode");
    NodeRegistry::instance().registerType<CameraNode>("Camera");
    NodeRegistry::instance().registerType<UINode>("UINode");
    NodeRegistry::instance().registerType<UICanvasNode>("UICanvasNode");
    NodeRegistry::instance().registerType<UITextNode>("UITextNode");

    std::string typeMatrixError;
    if (!verifyRegisteredRuntimeTypes(RuntimeTypeTarget::PlayerWeb, typeMatrixError))
        return failBoot("runtime type matrix mismatch: " + typeMatrixError);

    gApp.resources = std::make_unique<ResourceManager>(*gApp.device, gApp.registry.get());
    if (gVerifyRuntimeContract) {
        runtime::RoundTripContractReport report;
        std::string error;
        if (!runtime::verifyRuntimeRoundTripContract(
                RuntimeTypeTarget::PlayerWeb, *gApp.resources, report, error))
            return failBoot("player Web runtime contract: " + error);
        Log::info("[CONTRACT] PASS playerWeb nodes=", report.nodes,
                  " behaviours=", report.behaviours,
                  " properties=", report.reflectedProperties);
    }

    auto scene = std::make_unique<Scene>();
    const std::string scenePath = "/project/" + boot.manifest.mainScene;
    if (!SceneSerializer::loadIntoScene(*scene, *gApp.resources, scenePath)) {
        return failBoot("failed scene contract preflight for main scene " + scenePath);
    }

    // Mounts the persistent World — same sequence as Engine::mountWorld().
    gApp.tree = std::make_unique<SceneTree>(*gApp.resources);
    gApp.tree->setProjectRoot(gApp.project->rootPath());
    for (const auto& [name, value] : gApp.project->autoloads()) {
        if (hasSuffix(value, ".scene")) {
            if (!gApp.tree->registerAutoloadScene(
                    name, gApp.project->rootPath() + "/" + value))
                return failBoot("autoload '" + name + "' violates the runtime contract");
        }
        else if (hasSuffix(value, ".js") || hasSuffix(value, ".mjs"))
            gApp.tree->registerAutoloadScript(name, value);
        else if (!gApp.tree->registerAutoloadType(name, value))
            return failBoot("autoload '" + name + "' requires unsupported behaviour '" +
                            value + "'");
    }
    gApp.world = gApp.tree->mountWorld(std::move(scene));

    gApp.renderer = std::make_unique<Renderer>(*gApp.device, *gApp.surface, *gApp.resources);
    Time::setScale(1.0f);

    gApp.world->update(0.0f);  // populates the meshes/lights lists before reporting
    Log::info("saida-player: '", gApp.project->name(), "' started — ",
              gApp.world->meshes().size(), " meshes, ",
              gApp.world->lights().size(), " lights");
    return true;
}

void frame() {
    if (!gApp.running) return;

    // real dt clamped (for suspended tabs); Time applies the game time scale.
    const double now = emscripten_get_now();
    float realDt = 1.0f / 60.0f;
    if (gApp.lastMs > 0.0)
        realDt = float(std::min(now - gApp.lastMs, 250.0) / 1000.0);
    gApp.lastMs = now;
    Time::advance(realDt);

    gApp.resources->pumpAssetLoads();
    Input::sample();  // GLFW keyboard/mouse → actions, before the behaviours
    gApp.world->update(Time::delta());
    JsRuntime::instance().executePendingJobs();
    gApp.cameraDirector.update(*gApp.world, gApp.camera, Time::delta());

    gApp.tree->applyDeferred();
    gApp.tree->tickTimers(Time::delta());
    AudioManager::get().update();
    if (gApp.tree->quitRequested()) {
        Log::info("saida-player: quit requested — stopping the loop");
        gApp.running = false;
        emscripten_cancel_main_loop();
        return;
    }

    gApp.renderer->drawFrame(*gApp.world, gApp.camera, gApp.project.get());
}

} // namespace

extern "C" EMSCRIPTEN_KEEPALIVE const char* saida_player_status() {
    static std::string encoded;
    auto registeredNames = [](const auto& factories) {
        std::vector<std::string> names;
        names.reserve(factories.size());
        for (const auto& [name, factory] : factories) {
            (void)factory;
            names.push_back(name);
        }
        std::sort(names.begin(), names.end());
        return names;
    };

    nlohmann::json status = {
        {"schema", 1},
        {"runtime", "player-web"},
        {"state", gApp.state},
        {"ready", gApp.running && gApp.state == "ready"},
        {"scene", {
            {"schema", format::kSceneVersion},
            {"unsupportedTypePolicy", "reject"},
        }},
        {"supportedNodes", registeredNames(NodeRegistry::instance().factories())},
        {"supportedBehaviours",
         registeredNames(BehaviourRegistry::instance().factories())},
    };
    if (!gApp.startupError.empty()) status["error"] = gApp.startupError;
    encoded = status.dump();
    return encoded.c_str();
}

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--test-autoload" && i + 1 < argc)
            gTestAutoloads.emplace_back(argv[++i]);
        else if (std::string(argv[i]) == "--verify-runtime-contract")
            gVerifyRuntimeContract = true;
    }

    if (!glfwInit()) {
        gApp.state = "error";
        gApp.startupError = "GLFW initialization failed";
        std::printf("saida-player: GLFW initialization failed\n");
        return 1;
    }
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    GLFWwindow* window =
        glfwCreateWindow(int(kWidth), int(kHeight), "Saida player", nullptr, nullptr);
    if (!window) {
        gApp.state = "error";
        gApp.startupError = "window creation failed";
        std::printf("saida-player: window creation failed\n");
        glfwTerminate();
        return 1;
    }

    Input::bindRaw(window);  // also installs the browser touch backend

    // Each capability joins this mask once it becomes real.
    uint32_t capabilities =
        uint32_t(platform::Capability::Rendering) |
        uint32_t(platform::Capability::KeyboardMouse) |
        uint32_t(platform::Capability::ScriptGameplay) |
        uint32_t(platform::Capability::Physics) |
        uint32_t(platform::Capability::Audio) |
        uint32_t(platform::Capability::GameUI) |
        uint32_t(platform::Capability::UserStorage);
    if (Input::gamepadBackendAvailable())
        capabilities |= uint32_t(platform::Capability::GamepadInput);
    if (Input::touchBackendAvailable())
        capabilities |= uint32_t(platform::Capability::TouchInput);
    platform::setCapabilities(capabilities);
    AudioManager::get().init();
    Log::info(platform::report());

    rhi::Device::requestAsync([](std::unique_ptr<rhi::Device> device) {
        if (!device) {
            gApp.state = "error";
            gApp.startupError = "WebGPU unavailable";
            std::printf("saida-player: WebGPU unavailable\n");
            return;
        }
        try {
            gApp.device = std::move(device);
            gApp.surface =
                std::make_unique<rhi::Surface>(*gApp.device, "#canvas", kWidth, kHeight);
            registerBuiltinRenderFeatures();
            if (!bootGame()) return;
            gApp.running = true;
            gApp.state = "ready";
            // ?smoke: loop on a timer (30 fps) instead of rAF, so E2E test
            // harnesses can run in a hidden tab (rAF is suspended there).
            const int fps = EM_ASM_INT({
                return location.search.indexOf('smoke') >= 0 ? 30 : 0;
            });
            emscripten_set_main_loop(frame, fps, false);
        } catch (const std::exception& e) {
            failBoot(std::string("uncaught boot error: ") + e.what());
        }
    });

    emscripten_exit_with_live_runtime();
    return 0;
}
