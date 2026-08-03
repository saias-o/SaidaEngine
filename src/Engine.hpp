#pragma once

#include "core/Camera.hpp"
#include "render/CameraDirector.hpp"
#include "ui/UIInteractionSystem.hpp"

#include <functional>
#include <memory>
#include <string>

namespace saida {

class Window;
class VulkanDevice;
class Swapchain;
class ResourceManager;
class Scene;
class SceneTree;
class ImGuiLayer;
class Renderer;
class Project;
#ifdef SAIDA_ENABLE_XR
namespace xr { class Instance; class Session; }
class VulkanDeviceCreator;
#endif

using SceneSetup = std::function<void(Scene&, ResourceManager&)>;

// Member order keeps device_ alive until dependent GPU resources are destroyed.
class Engine {
public:
    explicit Engine(SceneSetup sceneSetup, const std::string& initialProject = "",
                    bool requireXr = false);
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    using FrameFn = std::function<void(float dt)>;
    void setOnFrame(FrameFn fn) { onFrame_ = std::move(fn); }

    void run();

    // The browser owns its main loop, so this must not block.
    bool tick();

    bool xrMode() const { return xrMode_; }

    void setSceneOverride(Scene* scene) { sceneOverride_ = scene; }
    void setRenderViewport(glm::vec2 position, glm::vec2 size);
    void clearRenderViewport();

    void mountWorld();
    void unmountWorld();

    // Write the composite frame (3D + UI) to `pngPath` once frame `afterFrames`
    // has been drawn (1-based: frame N is captured), then close the window. The
    // frame number is what makes a capture reproducible: the caller names the
    // frame it wants rather than racing the loop. A failure is reported and
    // closes the window too, so the process never hangs waiting for an image
    // that will not come.
    void captureFrameThenExit(const std::string& pngPath, uint32_t afterFrames);
    bool captureFailed() const { return captureFailed_; }

#ifdef SAIDA_ENABLE_XR
    // XR preview is isolated so the editor never owns OpenXR presentation.
    bool launchExternalPreviewIfNeeded();
#endif

    Scene& scene() { return *scene_; }
    SceneTree& sceneTree() { return *sceneTree_; }
    Camera& camera() { return camera_; }
    ResourceManager& resources() { return *resources_; }
    Project& project() { return *project_; }
    Window& window() { return *window_; }

private:
    void runDesktop();
    double tickLastTime_ = 0.0;
    bool tickWasLeftDown_ = false;

    // Deterministic frame capture (--screenshot). `capturePath_` empty means no
    // capture was asked for. Armed before the target frame is drawn, resolved
    // after, so the requested frame number is the one actually captured.
    void armFrameCapture();
    void serviceFrameCapture();
    std::string capturePath_;
    uint32_t captureAfterFrames_ = 0;
    uint32_t framesDrawn_ = 0;
    bool captureRequested_ = false;
    bool captureFailed_ = false;
#ifdef SAIDA_ENABLE_XR
    void runXr();
#endif

    std::unique_ptr<Window> window_;
#ifdef SAIDA_ENABLE_XR
    std::unique_ptr<xr::Instance> xrInstance_;
    std::unique_ptr<VulkanDeviceCreator> xrCreator_;
#endif
    std::unique_ptr<VulkanDevice> device_;
    std::unique_ptr<Swapchain> swapchain_;
    std::unique_ptr<ResourceManager> resources_;

    UIInteractionSystem uiInteraction_;

    std::unique_ptr<Scene> scene_;
    Scene* sceneOverride_ = nullptr;
    std::unique_ptr<SceneTree> sceneTree_;
    std::unique_ptr<ImGuiLayer> imgui_;
    std::unique_ptr<Renderer> renderer_;
    std::unique_ptr<Project> project_;

    FrameFn onFrame_;
    Camera camera_;
    CameraDirector cameraDirector_;  // picks + blends scene cameras during Play
    bool renderViewportOverride_ = false;
    glm::vec2 renderViewportPos_{0.0f};
    glm::vec2 renderViewportSize_{0.0f};

    std::string playSnapshot_;  // edit doc serialized at play start, to restore on stop

#ifdef SAIDA_ENABLE_XR
    // OpenXR session — declared last so it is destroyed first (before device_ and
    // xrInstance_), since it owns Vulkan command buffers / image views.
    std::unique_ptr<xr::Session> xrSession_;
#endif
    bool xrMode_ = false;
};

} // namespace saida
