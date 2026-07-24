#pragma once

#include <glm/glm.hpp>

#include <array>
#include <memory>
#include <vector>

#include "core/Camera.hpp"
#include "project/AssetRegistry.hpp"
#include "graphics/Material.hpp"     // MaterialType
#include "render/GpuDrivenLayout.hpp"
#include "render/RenderFeature.hpp"  // EyeRenderInfo, RenderContext, FrameContext, ScenePassFeature
#include "rhi/Rhi.hpp"

namespace saida {

class VulkanDevice;
class Window;
#ifndef SAIDA_RHI_WEBGPU
class ComputePipeline;
#endif
class ImGuiLayer;
class Scene;
class Camera;
class Mesh;
class Project;
class Material;
class ResourceManager;
class ShadowMap;
class UIRenderer;
class GIVolume;
class GpuProfiler;
class PostProcessor;
struct SceneSettings;

// Index 0 serves mono rendering; multiview selects an eye with gl_ViewIndex.
struct UniformBufferObject {
    alignas(16) glm::mat4 view[2];
    alignas(16) glm::mat4 proj[2];
};

// Must match PushConstants in the scene shaders.
struct PushConstants {
    glm::mat4 model;
    glm::vec4 params;  // y = bone matrix offset, or -1 when unskinned
};

constexpr int kMaxLights = 16;
constexpr int kMaxShadowCasters = 4;

// vec4/ivec4 avoid std140 padding ambiguities.
struct GpuLight {
    glm::vec4 posRange{0.0f};    // xyz world pos (point/spot), w = range
    glm::vec4 colorInt{0.0f};    // rgb color, w = intensity
    glm::vec4 dirType{0.0f};     // xyz direction (dir/spot), w = type (0 dir,1 point,2 spot)
    glm::vec4 spotShadow{0.0f, 0.0f, -1.0f, 0.0f};  // x cosInner, y cosOuter, z shadowIndex, w unused
};

struct LightingUBO {
    glm::vec4 ambient{0.0f};
    glm::vec4 cameraPos{0.0f};
    glm::vec4 shadowParams; // x = softness, yzw = unused
    glm::ivec4 counts{0};  // x = light count, y = mode (0 realtime, 1 baked)
    GpuLight lights[kMaxLights]{};
    glm::mat4 shadowMatrices[kMaxShadowCasters]{};  // light-space view-proj per shadow caster
    glm::vec4 giOrigin{0.0f};   // xyz = volume min corner (world), w = enabled (0/1)
    glm::vec4 giSpacing{1.0f};  // xyz = probe spacing
    glm::ivec4 giCounts{0};     // xyz = probe counts, w = probesPerRow in atlas
    glm::ivec4 giAtlas{0};      // x = irradiance texels/probe, y = visibility texels/probe
    glm::vec4 environmentParams{0.0f}; // x enabled, y diffuse, z specular, w rotation
};

class Renderer {
public:
    Renderer(rhi::Device& device, rhi::Surface& surface, Window& window,
             ResourceManager& resources, ImGuiLayer& imgui);
#ifdef SAIDA_RHI_WEBGPU
    Renderer(rhi::Device& device, rhi::Surface& surface, ResourceManager& resources);
#endif
#ifdef SAIDA_ENABLE_XR
    Renderer(VulkanDevice& device, Window& window, ResourceManager& resources,
             VkExtent2D xrEyeExtent, VkFormat xrColorFormat, uint32_t xrViewCount);
#endif
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    void drawFrame(Scene& scene, Camera& camera, Project* project);
    void setViewportRect(glm::vec2 position, glm::vec2 size);
    void clearViewportRect();

    // Viewer setting; does not alter per-light scene data.
    void setShadowsEnabled(bool enabled) { shadowsEnabled_ = enabled; }
    bool shadowsEnabled() const { return shadowsEnabled_; }

#ifdef SAIDA_ENABLE_XR
    void drawXr(VkCommandBuffer cmd, const std::vector<EyeRenderInfo>& eyes,
                Scene& scene, Project* project);
#endif

private:
    struct TonemapPushConstants {
        glm::mat4 invProjection{1.0f};
        glm::vec4 aoParams{0.0f};        // x enabled, y radius, z intensity, w power
        glm::vec4 fogColor{0.0f};
        glm::vec4 fogParams{0.0f};       // x enabled, y start, z density, w exposure
        glm::vec4 bloomParams{0.0f};     // x enabled, y threshold, z intensity, w radius px
        glm::vec4 sourceRect{0.0f, 0.0f, 1.0f, 1.0f};
        glm::vec4 projectionParams{0.0f};
        glm::vec4 projectionParams2{0.0f};
    };

    struct WebCanvasWorldPushConstants {
        glm::mat4 model{1.0f};
        glm::vec4 params{0.0f};  // x width, y height, z bindless texture index, w alpha
    };

    void updateGlobalShadowDescriptor();
    void updateGIDescriptors();  // re-point set 0 bindings 4/5 at the current GI atlas
    void updateEnvironmentDescriptor(Scene& scene);
    bool shouldUpdateRealtimeGI(bool dirty) const;
    uint64_t giDirtySignature(const Scene& scene) const;
    TonemapPushConstants tonemapPushConstants(const SceneSettings& settings,
                                              const glm::mat4& projection) const;
    void createGlobalSetLayout();
    void createPipeline(rhi::BindGroupLayout& materialSetLayout);
    void createWebCanvasWorldPipeline();
    // Layout-compatible variants keep existing binds valid across a switch.
    rhi::Pipeline* scenePipelineFor(MaterialType type) const {
        return (type == MaterialType::Unlit && unlitPipeline_) ? unlitPipeline_.get()
                                                               : pipeline_.get();
    }
    const std::vector<SceneDraw>& featureDraws() const;
    void createHdrResources();
    void cleanupHdrResources();
    void createTonemapPipeline();
    void updateTonemapDescriptorSet();
    void recordTonemapPass(rhi::CommandEncoder& encoder, uint32_t imageIndex,
                           Scene& scene, const Camera& camera);
    void createUniformBuffers();
    void createGlobalDescriptorSets();
    // Bind groups are immutable, so changed inputs require a new group.
    void rebuildGlobalSet(int frame);
    void createGpuDrivenBuffers();
    void createCullingPipeline();
    void uploadGpuDrivenDraws();

    void updateUniformBuffer(uint32_t frame, Scene& scene, Camera& camera, Project* project);
    // Keeps a CPU-visible fallback for features and oversized GPU frames.
    void gatherScene(LightingUBO& ubo, Scene& scene, const glm::vec3& cameraPos,
                     const Frustum* cullFrustum, Project* project,
                     const glm::mat4* view = nullptr, const glm::mat4* proj = nullptr);
    void recordMeshDraws(rhi::RenderPassEncoder& rp, rhi::Pipeline* firstPipeline, bool xrMultiview);
    void recordWorldWebCanvases(rhi::RenderPassEncoder& rp, Scene& scene, const Camera& camera);
    void recordShadowPasses(rhi::CommandEncoder& encoder);
    void recordCommandBuffer(rhi::CommandEncoder& encoder, uint32_t imageIndex, Scene& scene,
                             const Camera& camera);
    rhi::Rect2D activeRenderRect() const;

    rhi::Device& device_;
    rhi::Surface* swapchain_ = nullptr;  // null in XR mode (OpenXR owns presentation)
    Window* window_ = nullptr;
    ResourceManager& resources_;
    ImGuiLayer* imgui_ = nullptr;     // null in XR mode (no debug overlay yet)

    std::unique_ptr<rhi::Pipeline> pipeline_;        // scene pipeline: MaterialType::Lit
    std::unique_ptr<rhi::Pipeline> unlitPipeline_;   // scene pipeline: MaterialType::Unlit
    // Material type comes from the bindless material table.
    std::unique_ptr<rhi::Pipeline> gpuDrivenPipeline_;
    std::unique_ptr<rhi::Pipeline> webCanvasWorldPipeline_;
    std::unique_ptr<ShadowMap> shadowMap_;
    std::unique_ptr<UIRenderer> uiRenderer_;
    std::unique_ptr<GIVolume> gi_;  // DDGI irradiance volume (the single GI primitive)
    std::unique_ptr<GpuProfiler> gpuProfiler_;

    bool viewportOverride_ = false;
    glm::vec2 viewportPos_{0.0f};
    glm::vec2 viewportSize_{0.0f};

    std::unique_ptr<rhi::RenderTexture> hdrTexture_;
    std::unique_ptr<rhi::RenderTexture> hdrMsaaTexture_;       // MSAA scene target (when MSAA on)
    std::unique_ptr<rhi::RenderTexture> depthResolveTexture_;  // single-sample depth for AO when MSAA on
    std::unique_ptr<PostProcessor> postProcessor_;

    std::unique_ptr<rhi::Pipeline> tonemapPipeline_;
    std::unique_ptr<rhi::BindGroupLayout> tonemapSetLayout_;
    std::unique_ptr<rhi::BindGroup> tonemapSet_;
    std::unique_ptr<rhi::Sampler> tonemapSampler_;         // linear (HDR + bloom)
    std::unique_ptr<rhi::Sampler> tonemapDepthSampler_;    // nearest (AO depth)
    float exposure_ = 1.0f;

    // Registration order is draw order.
    std::vector<std::unique_ptr<ScenePassFeature>> features_;
    void buildFeatures(uint32_t viewMask, rhi::Format depthFormat,
                       rhi::SampleCount samples);
    void recordFeatures(FrameContext& fc);

    std::unique_ptr<rhi::BindGroupLayout> globalSetLayout_;
    std::vector<std::unique_ptr<rhi::BindGroup>> globalGroups_;
#ifdef SAIDA_RHI_WEBGPU
    // WebGPU rejects sampling a texture while the same pass storage-writes it.
    std::vector<std::unique_ptr<rhi::BindGroup>> giComputeGlobalGroups_;
#endif
    std::vector<std::unique_ptr<rhi::Buffer>> uniformBuffers_;
    std::vector<std::unique_ptr<rhi::Buffer>> lightingBuffers_;
    std::vector<std::unique_ptr<rhi::Buffer>> boneMatricesBuffers_;

    // Oversized frames fall back to CPU draws instead of being truncated.
    std::vector<std::unique_ptr<rhi::Buffer>> instanceBuffers_;
    std::vector<std::unique_ptr<rhi::Buffer>> originalDrawCommandBuffers_;
    std::vector<std::unique_ptr<rhi::Buffer>> drawCommandBuffers_;
    std::vector<std::unique_ptr<rhi::Buffer>> countBuffers_;
    static constexpr uint32_t kMaxInstances = 100000;
    // Skinning palette: 3x4 affine matrix per bone (3 vec4 rows, 48 bytes),
    // same layout in the Vulkan and WGSL shaders.
    static constexpr uint32_t kBoneRowsPerBone = 3;
    static constexpr uint32_t kBytesPerBone = kBoneRowsPerBone * sizeof(glm::vec4);
    static constexpr uint32_t kMaxPaletteBones = (4u * 1024u * 1024u) / kBytesPerBone;
    
#ifdef SAIDA_RHI_WEBGPU
    std::unique_ptr<rhi::ComputePipeline> cullingPipeline_;
#else
    std::unique_ptr<ComputePipeline> cullingPipeline_;
#endif
    std::unique_ptr<rhi::BindGroupLayout> cullingSetLayout_;
    std::vector<std::unique_ptr<rhi::BindGroup>> cullingGroups_;
    uint32_t currentInstanceCount_ = 0;
    bool gpuDrivenAvailable_ = false;
    bool gpuFrameActive_ = false;
    Frustum cameraFrustum_;

    uint32_t currentFrame_ = 0;

    struct FrameDrawLists {
        std::vector<SceneDraw> gpuCandidates;
        std::vector<SceneDraw> visibleDraws;
    } frameDraws_;
    struct ShadowDraw {
        Mesh* mesh = nullptr;
        glm::mat4 world{1.0f};
    };
    std::vector<ShadowDraw> shadowDraws_;

    std::array<glm::mat4, kMaxShadowCasters> shadowMatrices_{};
    int shadowCount_ = 0;
    bool shadowsEnabled_ = true;

    static constexpr int kGIBakeFrames = 256;
    static constexpr int kGIRealtimeWarmupFrames = 8;
    int giBakeFramesRemaining_ = 0;
    int giRealtimeWarmupRemaining_ = kGIRealtimeWarmupFrames;
    uint64_t giFrameCounter_ = 0;
    uint32_t giLastHierarchyVersion_ = 0;
    uint32_t giLastTransformVersion_ = 0;
    uint64_t giLastDirtySignature_ = 0;
    bool giWasEnabled_ = true;
    int giLastLightingMode_ = 0;
    int giLastMode_ = 0;
    bool giUpdateThisFrame_ = true;
    std::array<rhi::TextureView, 2> cachedGiIrradianceView_{};
    std::array<rhi::TextureView, 2> cachedGiVisibilityView_{};
    std::array<rhi::SamplerHandle, 2> cachedGiSampler_{};
    std::array<rhi::TextureView, 2> cachedEnvironmentView_{};
    std::array<rhi::SamplerHandle, 2> cachedEnvironmentSampler_{};

#ifdef SAIDA_ENABLE_XR
    bool xrMode_ = false;
    VkExtent2D xrExtent_{};
    VkFormat xrColorFormat_{};       // XR swapchain (sRGB) format
    uint32_t xrViewCount_ = 2;

    void createXrTargets();
    void createXrPipelines();
    void cleanupXrTargets();
    void updateXrTonemapDescriptorSets();
    void updateUniformBufferXr(uint32_t frame, const std::vector<EyeRenderInfo>& eyes,
                               Scene& scene, Project* project);
    void recordXrScenePass(rhi::CommandEncoder& encoder, Scene& scene,
                           const std::vector<EyeRenderInfo>& eyes);
    void recordXrWorldWebCanvases(rhi::RenderPassEncoder& rp, Scene& scene,
                                  const std::vector<EyeRenderInfo>& eyes);
    void recordXrTonemap(rhi::CommandEncoder& encoder, Scene& scene,
                         const std::vector<EyeRenderInfo>& eyes);

    std::unique_ptr<rhi::Pipeline> xrScenePipeline_;    // multiview scene: MaterialType::Lit
    std::unique_ptr<rhi::Pipeline> xrUnlitPipeline_;    // multiview scene: MaterialType::Unlit
    std::unique_ptr<rhi::Pipeline> xrWebCanvasWorldPipeline_;
    std::unique_ptr<rhi::Pipeline> xrTonemapPipeline_;  // per-eye tonemap → XR image
    rhi::Pipeline* xrScenePipelineFor(MaterialType type) const {
        return (type == MaterialType::Unlit && xrUnlitPipeline_) ? xrUnlitPipeline_.get()
                                                                 : xrScenePipeline_.get();
    }

    std::unique_ptr<rhi::RenderTexture> xrHdrTexture_;
    std::unique_ptr<rhi::RenderTexture> xrDepthTexture_;

    std::array<std::unique_ptr<rhi::BindGroup>, 2> xrTonemapSets_;   // one per eye layer
    std::array<std::unique_ptr<PostProcessor>, 2> xrPostProcessors_;
#else
    bool xrMode_ = false; // fallback for non-XR builds
#endif

    glm::mat4 lodView_{1.0f};
    glm::mat4 lodProj_{1.0f};
    bool lodMatricesValid_ = false;
};

} // namespace saida
