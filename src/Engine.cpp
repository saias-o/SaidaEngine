#include "Engine.hpp"

#include "core/Input.hpp"
#include "core/Log.hpp"
#include "core/Paths.hpp"
#include "core/Profiler.hpp"
#include "core/Time.hpp"
#include "core/Window.hpp"
#include "graphics/ImGuiLayer.hpp"
#include "graphics/MemoryProfiler.hpp"
#include "graphics/ResourceManager.hpp"
#include "graphics/Swapchain.hpp"
#include "graphics/VulkanDevice.hpp"
#include "project/Project.hpp"
#include "render/Renderer.hpp"
#include "scene/Scene.hpp"
#include "scene/SceneTree.hpp"
#include "scene/SceneSerializer.hpp"
#include "scene/BehaviourRegistry.hpp"
#include "scene/NodeRegistry.hpp"
#include "nodes/MeshNode.hpp"
#include "nodes/LightNode.hpp"
#include "nodes/CameraNode.hpp"
#include "physics/CollisionShapeNode.hpp"
#include "physics/StaticBodyNode.hpp"
#include "physics/RigidBodyNode.hpp"
#include "physics/AreaNode.hpp"
#include "physics/CharacterBodyNode.hpp"
#include "nodes/WebCanvasNode.hpp"
#include "nodes/UINode.hpp"
#include "nodes/UICanvasNode.hpp"
#include "nodes/UIColorNode.hpp"
#include "nodes/UIImageNode.hpp"
#include "nodes/UITextNode.hpp"
#include "nodes/UIInteractableNode.hpp"
#include "nodes/UIButtonNode.hpp"
#include "nodes/UIToggleNode.hpp"
#include "audio/AudioManager.hpp"
#include "audio/AudioSourceBehaviour.hpp"
#include "behaviours/CharacterBehaviour.hpp"
#include "behaviours/CameraFollowBehaviour.hpp"
#include "behaviours/SpawnerBehaviour.hpp"
#include "behaviours/RotatorBehaviour.hpp"
#include "scene/ReflectedTypes.hpp"
#include "scene/RuntimeTypeMatrix.hpp"
#include "render/RenderFeatureRegistry.hpp"
#include "ui/RmlUiRuntime.hpp"
#ifdef SAIDA_ENABLE_XR
#include "xr/XrInstance.hpp"
#include "xr/XrSession.hpp"
#include "xr/XrVulkanBinding.hpp"
#include "xr/XrRegistration.hpp"
#include "xr/toolkit/XRInput.hpp"
#include "xr/toolkit/XROrigin.hpp"
#endif

#include <exception>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <vector>
#include <stdexcept>
#include <thread>
#include <chrono>
#include <filesystem>

#ifdef _WIN32
#include <process.h>
extern "C" __declspec(dllimport) unsigned int __stdcall timeBeginPeriod(unsigned int period);
extern "C" __declspec(dllimport) unsigned int __stdcall timeEndPeriod(unsigned int period);
#ifndef TIMERR_NOERROR
#define TIMERR_NOERROR 0
#endif
#endif

namespace saida {

namespace {
constexpr uint32_t kWidth = 1600;
constexpr uint32_t kHeight = 900;
constexpr uint32_t kHiddenTestWidth = 640;
constexpr uint32_t kHiddenTestHeight = 360;

void sleepUntil(double targetTime) {
    while (true) {
        const double remaining = targetTime - glfwGetTime();
        if (remaining <= 0.0) break;

        // Windows can still coalesce a nominal 1 ms sleep into ~15.6 ms on some
        // systems. Stop sleeping early enough that one coarse sleep cannot push
        // a 45/60 FPS cap down to the next refresh-like bucket.
        if (remaining > 0.010) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } else {
            std::this_thread::yield();
        }
    }
}

#ifdef _WIN32
class TimerResolutionScope {
public:
    TimerResolutionScope() : active_(timeBeginPeriod(1) == TIMERR_NOERROR) {}
    ~TimerResolutionScope() {
        if (active_) timeEndPeriod(1);
    }

    TimerResolutionScope(const TimerResolutionScope&) = delete;
    TimerResolutionScope& operator=(const TimerResolutionScope&) = delete;

private:
    bool active_ = false;
};
#else
class TimerResolutionScope {
public:
    TimerResolutionScope() = default;
};
#endif
}

Engine::Engine(SceneSetup sceneSetup, const std::string& initialProject, bool requireXr) {
    // The XR process currently has no desktop mirror swapchain. Keep its host
    // GLFW window hidden instead of presenting a misleading unrendered surface.
    // CI may also hide it while exercising the exact editor Build/runtime path
    // on a clean runner with a software Vulkan ICD.
    const bool hiddenTestWindow = std::getenv("SAIDA_WINDOW_HIDDEN") != nullptr;
    const bool hideWindow = requireXr || hiddenTestWindow;
    const uint32_t width = hiddenTestWindow ? kHiddenTestWidth : kWidth;
    const uint32_t height = hiddenTestWindow ? kHiddenTestHeight : kHeight;
    window_ = std::make_unique<Window>(width, height, "SaidaEngine", !hideWindow);
    Input::bind(window_.get());

    // Process roles are explicit: the editor always owns a desktop Vulkan/ImGui
    // path, while the --xr preview process requires OpenXR from startup. A Vulkan
    // device cannot be converted from one presentation owner to the other later.
#ifdef SAIDA_ENABLE_XR
    if (requireXr) {
        try {
            xrInstance_ = std::make_unique<xr::Instance>();
            xrCreator_ = std::make_unique<xr::XrVulkanDeviceCreator>(*xrInstance_);
            device_ = std::make_unique<VulkanDevice>(*window_, xrCreator_.get());
            xrSession_ = std::make_unique<xr::Session>(*xrInstance_, *device_);
            xrMode_ = true;
            Log::info("XR mode active (OpenXR session)");
        } catch (const std::exception& e) {
            throw std::runtime_error(std::string("XR preview startup failed: ") + e.what());
        }
    }
#endif
    if (!device_) device_ = std::make_unique<VulkanDevice>(*window_);
    if (!xrMode_) swapchain_ = std::make_unique<Swapchain>(*device_, *window_);
    resources_ = std::make_unique<ResourceManager>(*device_);

    scene_ = std::make_unique<Scene>();
    project_ = std::make_unique<Project>();
    sceneTree_ = std::make_unique<SceneTree>(*resources_);

    resources_->setRegistry(&project_->assetRegistry());

    if (!initialProject.empty() && !project_->load(initialProject))
        Log::warn("Failed to load initial project: ", initialProject);

    if (!xrMode_ && swapchain_)
        swapchain_->setVSync(project_->vSync());

    // No project loaded → the game provides its scene.
    if (!project_->isLoaded() && sceneSetup)
        sceneSetup(*scene_, *resources_);

    AudioManager::get().init();
    
    // Reflection-described types (manifest + factories), incl. Rotator, Character,
    // LightNode. Single central list — see scene/ReflectedTypes.cpp.
    registerReflectedTypes();
    registerBuiltinRenderFeatures();  // water/skybox/debug-lines plug into the Renderer
#ifdef SAIDA_ENABLE_XR
    saida::xr::registerTypes();
#endif

    // Register built-in nodes
    NodeRegistry::instance().registerType<Node>("Node");
    NodeRegistry::instance().registerType<Scene>("Scene");
    NodeRegistry::instance().registerType<MeshNode>("MeshNode");
    // LightNode registered via registerReflectedTypes().
    NodeRegistry::instance().registerType<CameraNode>("Camera");
    NodeRegistry::instance().registerType<WebCanvasNode>("WebCanvasNode");
    NodeRegistry::instance().registerType<UINode>("UINode");
    NodeRegistry::instance().registerType<UICanvasNode>("UICanvasNode");
    NodeRegistry::instance().registerType<UIColorNode>("UIColorNode");
    NodeRegistry::instance().registerType<UIImageNode>("UIImageNode");
    NodeRegistry::instance().registerType<UITextNode>("UITextNode");
    NodeRegistry::instance().registerType<UIInteractableNode>("UIInteractableNode");
    NodeRegistry::instance().registerType<UIButtonNode>("UIButtonNode");
    NodeRegistry::instance().registerType<UIToggleNode>("UIToggleNode");
    NodeRegistry::instance().registerType<CollisionShapeNode>("CollisionShape");
    NodeRegistry::instance().registerType<StaticBodyNode>("StaticBody");
    NodeRegistry::instance().registerType<RigidBodyNode>("RigidBody");
    // "Area" is registered by registerReflectedTypes() (reflected signals).
    NodeRegistry::instance().registerType<CharacterBodyNode>("CharacterBody");
    // XR Toolkit nodes/behaviours are registered by xr::registerTypes() above.
    std::string typeMatrixError;
    if (!verifyRegisteredRuntimeTypes(RuntimeTypeTarget::Native, typeMatrixError))
        throw std::runtime_error("runtime type matrix mismatch: " + typeMatrixError);
    if (project_->isLoaded()) {
        AudioManager::get().setProjectRoot(project_->rootPath());
        AudioManager::get().setDefaultSettings(project_->defaultAudioSettings());
        AudioManager::get().setMasterVolume(project_->masterVolume());
    }

    // Desktop present path: ImGui overlay + Renderer driving the window swapchain.
    // In XR mode the OpenXR session owns presentation, and the Renderer is built
    // for stereo multiview (2-layer targets sized to one eye, no ImGui/swapchain).
    if (!xrMode_) {
        imgui_ = std::make_unique<ImGuiLayer>(*device_, *window_, swapchain_->colorFormat(),
            swapchain_->imageCount(), VK_SAMPLE_COUNT_1_BIT);
        renderer_ = std::make_unique<Renderer>(*device_, *swapchain_, *window_,
            *resources_, *imgui_);
    }
#ifdef SAIDA_ENABLE_XR
    else {
        renderer_ = std::make_unique<Renderer>(*device_, *window_, *resources_,
            xrSession_->eyeExtent(), static_cast<VkFormat>(xrSession_->colorFormat()),
            xrSession_->viewCount());
    }
#endif

    // Default editor/viewport camera placement (the app may move it).
    camera_.position = {0.0f, 2.5f, 8.0f};
    camera_.yaw = -90.0f;
    camera_.pitch = -15.0f;
}

#ifdef SAIDA_ENABLE_XR
bool Engine::launchExternalPreviewIfNeeded() {
    bool xrScene = false;
    scene_->traverse([&](Node& node, const glm::mat4&) {
        if (dynamic_cast<XROrigin*>(&node)) xrScene = true;
    });
    if (!xrScene) return false;

    if (!project_->isLoaded()) {
        Log::error("XR Preview requires a loaded .saidaproj project");
        return true;
    }

    const std::filesystem::path xrState = applicationStatePath("xr");
    std::error_code pathError;
    std::filesystem::create_directories(xrState, pathError);
    if (pathError) {
        Log::error("XR Preview could not create state directory: ", pathError.message());
        return true;
    }
    const std::filesystem::path scenePath = xrState / "xr_preview.scene";
    if (!SceneSerializer::saveToFile(*scene_, *resources_, scenePath.string())) {
        Log::error("XR Preview could not serialize the current scene");
        return true;
    }

    const std::filesystem::path executable =
        std::filesystem::path(runtimeBinaryRoot()) / "SaidaEngine.exe";
    if (!std::filesystem::exists(executable)) {
        Log::error("XR Preview executable not found: ", executable.string());
        return true;
    }

    // MinGW's spawn command-line quoting is not reliable for arguments containing
    // spaces. Keep the child command line path-free and transfer launch data via
    // a fixed manifest next to the executable.
    const std::filesystem::path manifestPath = xrState / "xr_preview.launch";
    {
        std::ofstream manifest(manifestPath, std::ios::trunc);
        if (!manifest) {
            Log::error("XR Preview could not create launch manifest: ",
                       manifestPath.string());
            return true;
        }
        manifest << project_->filePath() << '\n' << scenePath.string() << '\n';
    }

#ifdef _WIN32
    std::vector<std::string> arguments = {
        executable.string(), "--xr-preview"};
    std::vector<const char*> argv;
    argv.reserve(arguments.size() + 1);
    for (const std::string& argument : arguments) argv.push_back(argument.c_str());
    argv.push_back(nullptr);

    const intptr_t child = _spawnv(_P_NOWAIT, executable.string().c_str(), argv.data());
    if (child == -1) {
        Log::error("XR Preview process launch failed: ", std::strerror(errno));
        return true;
    }
    std::thread([child] {
        int exitCode = 0;
        _cwait(&exitCode, child, _WAIT_CHILD);
    }).detach();
    Log::info("XR Preview launched (process ", child, ")");
#else
    Log::error("XR Preview process launch is currently implemented for Windows only");
#endif  // _WIN32
    return true;
}
#endif  // SAIDA_ENABLE_XR

Engine::~Engine() {
    unmountWorld();  // tear down the World (and its physics) before subsystems
    scene_.reset();
    sceneOverride_ = nullptr;
    sceneTree_.reset();
    // Every RmlUi context must die before Rml::Shutdown(): the scene's
    // WebCanvasNodes went with scene_, and the renderer's UI HUD context goes
    // with renderer_. Wait for the GPU first so nothing is still sampling the
    // renderer's resources, then drop it while the device is still alive.
    vkDeviceWaitIdle(device_->device());
    renderer_.reset();
    RmlUiRuntime::shutdown();
    AudioManager::get().shutdown();
    // Remaining subsystems are torn down by their unique_ptr destructors, in
    // reverse declaration order, while device_ is still alive (imgui_,
    // swapchain_ and device_).
}

void Engine::mountWorld() {
    cameraDirector_.reset();  // first scene camera snaps in (no blend from stale state)
    sceneTree_->setProjectRoot(project_->rootPath());  // resolve relative .scene paths

    // Register the project's data-driven autoloads (idempotent: dedup by name).
    // A value ending in ".scene" is a prefab path, ".js"/".mjs" a script;
    // otherwise a behaviour type.
    for (const auto& [name, value] : project_->autoloads()) {
        auto endsWith = [&value](const char* suffix) {
            const size_t n = std::char_traits<char>::length(suffix);
            return value.size() > n &&
                   value.compare(value.size() - n, n, suffix) == 0;
        };
        if (endsWith(".scene")) {
            std::filesystem::path p(value);
            std::string path = p.is_absolute() ? value : (project_->rootPath() + "/" + value);
            sceneTree_->registerAutoloadScene(name, path);
        } else if (endsWith(".js") || endsWith(".mjs")) {
            sceneTree_->registerAutoloadScript(name, value);
        } else {
            sceneTree_->registerAutoloadType(name, value);
        }
    }

    // Keep a snapshot to restore the edit doc on Stop, then MOVE the live scene
    // into the World as the current sub-scene — moving (not copying) means a
    // single set of live resources (no duplicate WebCanvas views, audio, etc.).
    playSnapshot_ = SceneSerializer::nodeToJson(*scene_, *resources_);
    std::unique_ptr<Scene> live = std::move(scene_);
    scene_ = std::make_unique<Scene>();  // placeholder while the doc is "in play"

    Scene* world = sceneTree_->mountWorld(std::move(live));
    setSceneOverride(world);
}

void Engine::unmountWorld() {
    if (!sceneTree_->mounted()) return;
    setSceneOverride(nullptr);
    sceneTree_->unmountWorld();  // destroys the World (and the moved/loaded sub-scene)

    // Rebuild the edit document from the snapshot taken at play start.
    if (!playSnapshot_.empty()) {
        if (auto restored = SceneSerializer::nodeFromJson(
                playSnapshot_, *resources_, NodeIdPolicy::Preserve))
            scene_.reset(static_cast<Scene*>(restored.release()));
        playSnapshot_.clear();
    }
}

void Engine::run() {
#ifdef SAIDA_ENABLE_XR
    if (xrMode_) { runXr(); return; }
#endif
    runDesktop();
}

void Engine::setRenderViewport(glm::vec2 position, glm::vec2 size) {
    renderViewportOverride_ = size.x > 1.0f && size.y > 1.0f;
    renderViewportPos_ = position;
    renderViewportSize_ = size;
    if (renderer_) renderer_->setViewportRect(position, size);
}

void Engine::clearRenderViewport() {
    renderViewportOverride_ = false;
    renderViewportPos_ = glm::vec2(0.0f);
    renderViewportSize_ = glm::vec2(0.0f);
    if (renderer_) renderer_->clearViewportRect();
}

void Engine::setCameraOverride(const glm::vec3& position, const glm::vec3& target) {
    if (position == target) {
        // lookAt on a zero-length direction yields a NaN orientation and a black
        // frame — a failure that reads as a rendering bug, so it is refused here.
        Log::error("[capture] --camera-pos and --camera-look are the same point; "
                   "the camera has nothing to aim at");
        captureFailed_ = true;
        return;
    }
    cameraOverridePosition_ = position;
    cameraOverrideTarget_ = target;
    cameraOverride_ = true;
}

void Engine::captureFrameThenExit(CaptureRequest request) {
    // Something already went wrong setting this run up (a degenerate viewpoint,
    // say). Arming anyway would leave a plausible PNG next to a non-zero exit
    // code, and a caller that checks the file rather than the code would take it
    // for the image it asked for.
    if (captureFailed_) return;

    if (request.fixedStep > 0.0f) setFixedTimestep(request.fixedStep);

    std::string error;
    if (!captureScheduler_.arm(std::move(request), error)) {
        Log::error("[capture] ", error);
        captureFailed_ = true;
    }
}

// Called immediately before the frame is drawn. The request must be in place
// while the target frame is recorded, which is what makes "--after-frames N
// captures frame N" true rather than approximately true.
void Engine::armFrameCapture() {
    if (!captureScheduler_.armed()) return;

    const CaptureScheduler::Decision decision =
        captureScheduler_.beforeFrame(resources_->assetLoadsSettled());
    if (decision == CaptureScheduler::Decision::Wait) return;

    if (decision == CaptureScheduler::Decision::Abandon) {
        Log::error("[capture] ", captureScheduler_.abandonReason());
        captureFailed_ = true;
        window_->close();
        return;
    }

    capturePath_ = captureScheduler_.request().pngPath;
    std::string error;
    if (!renderer_->requestCapture(capturePath_, error)) {
        Log::error("[capture] ", error);
        captureFailed_ = true;
        capturePath_.clear();
        window_->close();
        return;
    }
    captureRequested_ = true;
}

// Called after the frame is drawn: the copy is now recorded, so the image can
// be read back and written. The window closes either way, so a failed capture
// cannot leave the process spinning on an image that will never arrive.
void Engine::serviceFrameCapture() {
    if (capturePath_.empty() || !captureRequested_) return;
    if (!renderer_->capturePending()) return;

    std::string error;
    if (renderer_->resolveCapture(error)) {
        Log::info("[capture] wrote ", capturePath_, " (frame ",
                  captureScheduler_.framesCounted(), " after ",
                  captureScheduler_.framesWaited(), " settle frames, ",
                  framesDrawn_, " drawn)");
    } else {
        Log::error("[capture] ", error);
        captureFailed_ = true;
    }
    capturePath_.clear();
    window_->close();
}

void Engine::runDesktop() {
    TimerResolutionScope timerResolution;
    Profiler::instance().setThreadName("Main");
    tickLastTime_ = glfwGetTime();
    while (tick()) {}
    device_->waitIdle();
}

bool Engine::tick() {
    if (window_->shouldClose()) return false;
    SAIDA_PROFILE_FRAME_BEGIN();
    {
        SAIDA_PROFILE_SCOPE("Frame");

        {
            SAIDA_PROFILE_SCOPE("Window/PollEvents");
            window_->pollEvents();
        }

        const double frameStart = glfwGetTime();
        float realDt = static_cast<float>(frameStart - tickLastTime_);
        tickLastTime_ = frameStart;

        // A pinned clock replaces the measured frame time everywhere, including
        // the application callback: an editor camera easing on real time would
        // otherwise reintroduce the very variation the pin removes.
        if (fixedTimestep_ > 0.0f) realDt = fixedTimestep_;

        Time::update(realDt);  // sets scaled delta + elapsed
        resources_->pumpAssetLoads();
        Input::newFrame();     // single per-frame input snapshot

        bool isLeftDown = Input::isMouseButtonDown(MouseButton::Left);
        bool isLeftPressed = Input::isMouseButtonPressed(MouseButton::Left);
        bool isLeftReleased = !isLeftDown && tickWasLeftDown_;
        tickWasLeftDown_ = isLeftDown;

        Scene* activeScene = sceneOverride_ ? sceneOverride_ : scene_.get();

        int framebufferWidth = 0;
        int framebufferHeight = 0;
        window_->framebufferSize(framebufferWidth, framebufferHeight);
        glm::vec2 uiMouse = Input::mousePosition();
        glm::vec2 uiViewportSize{static_cast<float>(framebufferWidth), static_cast<float>(framebufferHeight)};
        if (renderViewportOverride_) {
            uiMouse -= renderViewportPos_;
            uiViewportSize = renderViewportSize_;
        }
        {
            SAIDA_PROFILE_SCOPE("UI/Interaction");
            if (uiInteraction_.update(*activeScene, camera_, uiMouse, uiViewportSize,
                                      isLeftDown, isLeftPressed, isLeftReleased)) {
                Input::consumeMouse();
            }
        }

        {
            SAIDA_PROFILE_SCOPE("ImGui/BeginFrame");
            imgui_->beginFrame();
        }
        if (onFrame_) {
            SAIDA_PROFILE_SCOPE("Editor/OnFrame");
            onFrame_(realDt);              // application: its input + UI
        }

        // The application callback may switch Play/Preview mode and destroy the
        // previously active scene. Never carry that borrowed pointer across the
        // callback boundary.
        activeScene = sceneOverride_ ? sceneOverride_ : scene_.get();
        {
            SAIDA_PROFILE_SCOPE("Scene/Update");
            activeScene->update(Time::delta());     // behaviours: scaled time (pausable)
        }

        // During Play, scene cameras drive the view (the editor fly cam is frozen).
        // The director picks the highest-priority active CameraNode and blends; with
        // no camera it returns false and the editor camera stays in control.
        if (sceneTree_->mounted()) {
            SAIDA_PROFILE_SCOPE("Scene/CameraDirector");
            cameraDirector_.update(*activeScene, camera_, Time::delta());
        }

        // Apply deferred gameplay ops (queueFree, changeScene) once behaviours are
        // done — never mutate the tree mid-update. The World object identity is
        // stable across scene swaps, so sceneOverride_ stays valid.
        if (sceneTree_->mounted()) {
            SAIDA_PROFILE_SCOPE("SceneTree/Deferred");
            sceneTree_->applyDeferred();
            sceneTree_->tickTimers(Time::delta());  // scaled dt → frozen on pause
            if (sceneTree_->quitRequested()) window_->close();
        }

        {
            SAIDA_PROFILE_SCOPE("Audio/Update");
            AudioManager::get().update();      // update audio spatialization
        }
        {
            SAIDA_PROFILE_SCOPE("ImGui/EndFrame");
            imgui_->endFrame();  // finalize draw data even if the frame is skipped (resize)
        }

        if (Profiler::instance().enabled()) {
            MemoryProfiler::publish(*device_);
        }
        // Last word on the camera, after the director and the editor have had
        // theirs: an inspection viewpoint is only useful if nothing overrides it.
        if (cameraOverride_) {
            camera_.position = cameraOverridePosition_;
            camera_.lookAt(cameraOverrideTarget_);
        }
        armFrameCapture();
        {
            SAIDA_PROFILE_SCOPE("Renderer/DrawFrame");
            renderer_->drawFrame(*activeScene, camera_, project_.get());
        }
        ++framesDrawn_;
        serviceFrameCapture();

        // Pacing a fictional clock only makes a capture slower: the frame time
        // it would preserve is no longer what the world advances by.
        const int maxFps = fixedTimestep_ > 0.0f
                               ? 0
                               : (project_ ? project_->maxFps() : Project::kDefaultMaxFps);
        if (maxFps > 0) {
            SAIDA_PROFILE_SCOPE("Frame/Throttle");
            sleepUntil(frameStart + 1.0 / static_cast<double>(maxFps));
        }
    }
    SAIDA_PROFILE_FRAME_END();
    return !window_->shouldClose();
}

#ifdef SAIDA_ENABLE_XR
void Engine::runXr() {
    // XR loop: paced by xrWaitFrame (inside renderFrame), not GLFW. The window
    // still exists (mirror/host) so we keep pumping it to stay responsive.
    double last = glfwGetTime();
    double lastPoseLog = last;
    while (!window_->shouldClose()) {
        window_->pollEvents();
        if (!xrSession_->pollEvents()) break;   // EXITING / loss pending

        double now = glfwGetTime();
        float realDt = static_cast<float>(now - last);
        last = now;
        Time::update(realDt);
        Input::newFrame();
        XRInput::beginFrame();
        xrSession_->syncActions();
        {
            XRPose head;
            head.tracked = true;
            head.position = xrSession_->headPosition();
            head.orientation = xrSession_->headOrientation();
            XRInput::submitHead(head);
        }

        Scene* activeScene = sceneOverride_ ? sceneOverride_ : scene_.get();
        activeScene->update(Time::delta());

        if (sceneTree_->mounted()) {
            sceneTree_->applyDeferred();
            sceneTree_->tickTimers(Time::delta());
            if (sceneTree_->quitRequested()) break;
        }
        AudioManager::get().update();

        {
            XROrigin* origin = nullptr;
            activeScene->traverse([&](Node& n, const glm::mat4&) {
                if (!origin)
                    if (auto* o = dynamic_cast<XROrigin*>(&n)) origin = o;
            });
            if (origin)
                xrSession_->setReferenceOffset(origin->transform().position,
                                               glm::radians(origin->yawDegrees));
        }

        camera_.position = xrSession_->headPosition();
        if (now - lastPoseLog > 1.0) {
            lastPoseLog = now;
            glm::vec3 p = xrSession_->headPosition();
            Log::info("XR head pos: ", p.x, ", ", p.y, ", ", p.z);
        }

        xrSession_->renderFrame(
            [&](VkCommandBuffer cmd, const std::vector<xr::EyeView>& eyes) {
                std::vector<EyeRenderInfo> eyeInfos;
                eyeInfos.reserve(eyes.size());
                for (const auto& e : eyes)
                    eyeInfos.push_back({e.image, e.imageView, e.extent, e.view, e.projection, e.eyePosition});
                renderer_->drawXr(cmd, eyeInfos, *activeScene, project_.get());
            });
    }
    vkDeviceWaitIdle(device_->device());
}
#endif  // SAIDA_ENABLE_XR

} // namespace saida
