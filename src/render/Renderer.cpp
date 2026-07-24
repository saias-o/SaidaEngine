#include "render/Renderer.hpp"

#include "core/Camera.hpp"
#include "core/Paths.hpp"
#include "project/Project.hpp"
#include "graphics/Buffer.hpp"
#include "core/Profiler.hpp"
#include "graphics/GpuProfiler.hpp"
#include "graphics/ImGuiLayer.hpp"
#include "graphics/Material.hpp"
#include "graphics/Mesh.hpp"
#include "graphics/Pipeline.hpp"
#include "graphics/ShadowMap.hpp"
#include "graphics/ResourceManager.hpp"
#include "render/GIVolume.hpp"
#include "render/PostProcessor.hpp"
#include "graphics/UIRenderer.hpp"
#include "nodes/LightNode.hpp"
#include "scene/Node.hpp"
#include "scene/Scene.hpp"
#include "core/Time.hpp"
#include "render/RenderFeatureRegistry.hpp"
#include "core/Log.hpp"

#ifndef SAIDA_RHI_WEBGPU
#include "graphics/MemoryProfiler.hpp"
#include "graphics/Swapchain.hpp"
#include "graphics/VulkanDevice.hpp"
#include "rhi/vulkan/Format.hpp"
#endif

#include "nodes/LightNode.hpp"
#include "nodes/MeshNode.hpp"
#include "scene/MeshLod.hpp"
#ifndef SAIDA_RHI_WEBGPU
#include "nodes/WebCanvasNode.hpp"
#endif
#include "scene/animation/Animator.hpp"
#ifdef SAIDA_ENABLE_XR
#include "xr/XrSession.hpp"   // xr::EyeView
#include "xr/toolkit/XRPassthrough.hpp"
#endif

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <unordered_map>
#include "graphics/ComputePipeline.hpp"
#include "graphics/Texture.hpp"
#include <stdexcept>

namespace saida {

namespace {
constexpr int kMaxFramesInFlight = 2;

constexpr float kDefaultShadowDistance = 25.0f;
constexpr float kSafeUpCollinearityThreshold = 0.99f;
constexpr float kHighGiProbeSpacing = 1.2f;
constexpr float kMediumGiProbeSpacing = 1.5f;
constexpr float kLowGiProbeSpacing = 2.4f;
constexpr float kMinAoPower = 0.001f;
constexpr float kMinBoundsRadius = 0.001f;
constexpr float kUnitCubeBoundsRadius = 0.866f; // approximately sqrt(3) / 2
constexpr uint32_t kAllTextureLayers = ~0u;

// Picks an up vector not colinear with the light direction.
glm::vec3 safeUp(const glm::vec3& dir) {
    return std::abs(dir.y) > kSafeUpCollinearityThreshold
               ? glm::vec3(0.0f, 0.0f, 1.0f)
               : glm::vec3(0.0f, 1.0f, 0.0f);
}

glm::mat4 directionalMatrix(const glm::vec3& dir, float distance) {
    float halfSize = distance;
    float depth = distance * 4.0f;
    glm::vec3 eye = -dir * (depth * 0.5f);
    glm::mat4 view = glm::lookAt(eye, glm::vec3(0.0f), safeUp(dir));
    glm::mat4 proj = glm::ortho(-halfSize, halfSize,
                                -halfSize, halfSize,
                                0.1f, depth);
    proj[1][1] *= -1.0f;
    return proj * view;
}

#ifndef SAIDA_RHI_WEBGPU
struct WebCanvasWorldDraw {
    WebCanvasNode* node = nullptr;
    float distance = 0.0f;
};

std::vector<WebCanvasWorldDraw> collectWorldWebCanvasDraws(Scene& scene, glm::vec3 viewpoint) {
    std::vector<WebCanvasWorldDraw> draws;
    for (WebCanvasNode* node : scene.webCanvases()) {
        if (!node) continue;
        if (node->mode() != WebCanvasNode::Mode::WorldSpace || !node->texture()) continue;
        glm::vec3 center = glm::vec3(node->worldTransform() * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
        draws.push_back({node, glm::length(center - viewpoint)});
    }
    std::stable_sort(draws.begin(), draws.end(), [](const WebCanvasWorldDraw& a, const WebCanvasWorldDraw& b) {
        if (a.node->renderOrder() != b.node->renderOrder()) {
            return a.node->renderOrder() < b.node->renderOrder();
        }
        return a.distance > b.distance;
    });
    return draws;
}
#endif

// Quality tiers trade GI fidelity for a fixed world coverage.
GIVolumeDesc giDescForTier(QualityTier tier) {
    GIVolumeDesc d;
    switch (tier) {
        case QualityTier::Ultra:
        case QualityTier::High:
            d.counts = {20, 10, 20};
            d.spacing = glm::vec3(kHighGiProbeSpacing);
            d.raysPerProbe = 96; d.voxelResolution = 96;
            break;
        case QualityTier::Medium:
            d.counts = {16, 8, 16};
            d.spacing = glm::vec3(kMediumGiProbeSpacing);
            d.raysPerProbe = 64; d.voxelResolution = 80;
            break;
        case QualityTier::Low:
        default:
            d.counts = {10, 5, 10};
            d.spacing = glm::vec3(kLowGiProbeSpacing);
            d.raysPerProbe = 48; d.voxelResolution = 64;
            break;
    }
    d.origin = -0.5f * glm::vec3(d.counts - 1) * d.spacing;
    return d;
}

#ifndef SAIDA_RHI_WEBGPU
uint32_t sampleCountValue(VkSampleCountFlagBits samples) {
    switch (samples) {
        case VK_SAMPLE_COUNT_2_BIT: return 2;
        case VK_SAMPLE_COUNT_4_BIT: return 4;
        case VK_SAMPLE_COUNT_8_BIT: return 8;
        case VK_SAMPLE_COUNT_16_BIT: return 16;
        case VK_SAMPLE_COUNT_32_BIT: return 32;
        case VK_SAMPLE_COUNT_64_BIT: return 64;
        case VK_SAMPLE_COUNT_1_BIT:
        default: return 1;
    }
}
#endif

void hashCombine(uint64_t& h, uint64_t v) {
    h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
}

void hashFloat(uint64_t& h, float v) {
    uint32_t bits = 0;
    static_assert(sizeof(bits) == sizeof(v), "float hash expects 32-bit floats");
    std::memcpy(&bits, &v, sizeof(bits));
    hashCombine(h, bits);
}

void hashVec4(uint64_t& h, const glm::vec4& v) {
    hashFloat(h, v.x);
    hashFloat(h, v.y);
    hashFloat(h, v.z);
    hashFloat(h, v.w);
}

void hashMat4(uint64_t& h, const glm::mat4& m) {
    for (int c = 0; c < 4; ++c) hashVec4(h, m[c]);
}

// Light-space view-proj for a spot light (perspective from its cone), Vulkan-flipped.
glm::mat4 spotMatrix(const glm::vec3& pos, const glm::vec3& dir, float outerAngleDeg, float range) {
    float fovy = glm::clamp(glm::radians(outerAngleDeg) * 2.0f, glm::radians(10.0f), glm::radians(170.0f));
    glm::mat4 view = glm::lookAt(pos, pos + dir, safeUp(dir));
    glm::mat4 proj = glm::perspective(fovy, 1.0f, 0.1f, std::max(range, 1.0f));
    proj[1][1] *= -1.0f;
    return proj * view;
}
}

Renderer::Renderer(rhi::Device& device, rhi::Surface& swapchain, Window& window,
                   ResourceManager& resources, ImGuiLayer& imgui)
    : device_(device), swapchain_(&swapchain), window_(&window), resources_(resources), imgui_(&imgui) {
    createGlobalSetLayout();
#ifdef SAIDA_RHI_WEBGPU
    (void)imgui;
#else
    gpuDrivenAvailable_ = device_.capabilities().descriptorIndexing &&
                          device_.capabilities().multiDrawIndirect &&
                          resources_.globalMaterialSetLayout() != VK_NULL_HANDLE &&
                          resources_.globalMaterialSet() != VK_NULL_HANDLE;
    if (gpuDrivenAvailable_) {
        createGpuDrivenBuffers();
        createCullingPipeline();
        Log::info("Renderer: GPU-driven scene submission enabled.");
    }
    uiRenderer_ = std::make_unique<UIRenderer>(device_, resources_, rhi::vulkan::fromVk(swapchain_->colorFormat()));
#endif
    createHdrResources();
    createPipeline(resources_.materialSetLayout());
    createWebCanvasWorldPipeline();
    createTonemapPipeline();
#ifdef SAIDA_RHI_WEBGPU
    buildFeatures(0, swapchain_->depthFormat(), swapchain_->samples());
#else
    buildFeatures(0, rhi::vulkan::fromVk(swapchain_->depthFormat()), swapchain_->samples());
#endif
    createUniformBuffers();
    shadowMap_ = std::make_unique<ShadowMap>(device_);
    gi_ = std::make_unique<GIVolume>(device_, giDescForTier(device_.capabilities().tier),
                                     resources_.materialSetLayout(), *globalSetLayout_);
    createGlobalDescriptorSets();
    gpuProfiler_ = std::make_unique<GpuProfiler>(device_, kMaxFramesInFlight);
}

#ifdef SAIDA_RHI_WEBGPU
Renderer::Renderer(rhi::Device& device, rhi::Surface& swapchain, ResourceManager& resources)
    : device_(device), swapchain_(&swapchain), resources_(resources) {
    createGlobalSetLayout();
    uiRenderer_ = std::make_unique<UIRenderer>(device_, resources_, swapchain_->colorFormat());
    createHdrResources();
    createPipeline(resources_.materialSetLayout());
    createWebCanvasWorldPipeline();
    createTonemapPipeline();
    buildFeatures(0, swapchain_->depthFormat(), swapchain_->samples());
    createUniformBuffers();
    shadowMap_ = std::make_unique<ShadowMap>(device_);
    gi_ = std::make_unique<GIVolume>(device_, giDescForTier(device_.capabilities().tier),
                                     resources_.materialSetLayout(), *globalSetLayout_);
    createGlobalDescriptorSets();
    gpuProfiler_ = std::make_unique<GpuProfiler>(device_, kMaxFramesInFlight);
}
#endif

#ifdef SAIDA_ENABLE_XR
Renderer::Renderer(VulkanDevice& device, Window& window, ResourceManager& resources,
                   VkExtent2D xrEyeExtent, VkFormat xrColorFormat, uint32_t xrViewCount)
    : device_(device), window_(&window), resources_(resources),
      xrMode_(true), xrExtent_(xrEyeExtent), xrColorFormat_(xrColorFormat),
      xrViewCount_(xrViewCount) {
    createGlobalSetLayout();
    createUniformBuffers();
    shadowMap_ = std::make_unique<ShadowMap>(device_);
    gi_ = std::make_unique<GIVolume>(device_, giDescForTier(device_.capabilities().tier),
                                     resources_.materialSetLayout(), *globalSetLayout_);
    createGlobalDescriptorSets();
    createXrTargets();
    createXrPipelines();
    gpuProfiler_ = std::make_unique<GpuProfiler>(device_, kMaxFramesInFlight);
}
#endif

Renderer::~Renderer() {
    device_.waitIdle();
    features_.clear();  // destroy feature pipelines/descriptors while the device is valid
#ifdef SAIDA_ENABLE_XR
    if (xrMode_) {
        for (auto& post : xrPostProcessors_) post.reset();
        cleanupXrTargets();
    }
#endif
    cleanupHdrResources();

}

void Renderer::setViewportRect(glm::vec2 position, glm::vec2 size) {
    viewportPos_ = position;
    viewportSize_ = size;
    viewportOverride_ = size.x > 1.0f && size.y > 1.0f;
}

void Renderer::clearViewportRect() {
    viewportOverride_ = false;
    viewportPos_ = glm::vec2(0.0f);
    viewportSize_ = glm::vec2(0.0f);
}

rhi::Rect2D Renderer::activeRenderRect() const {
    rhi::Extent2D full = swapchain_ ? swapchain_->extent() : rhi::Extent2D{1, 1};
    if (!viewportOverride_) return {{0, 0}, full};

    int32_t x = static_cast<int32_t>(std::max(0.0f, std::floor(viewportPos_.x)));
    int32_t y = static_cast<int32_t>(std::max(0.0f, std::floor(viewportPos_.y)));
    uint32_t w = static_cast<uint32_t>(std::max(1.0f, std::round(viewportSize_.x)));
    uint32_t h = static_cast<uint32_t>(std::max(1.0f, std::round(viewportSize_.y)));

    if (static_cast<uint32_t>(x) >= full.width || static_cast<uint32_t>(y) >= full.height) {
        return {{0, 0}, full};
    }
    w = std::min(w, full.width - static_cast<uint32_t>(x));
    h = std::min(h, full.height - static_cast<uint32_t>(y));
    return {{x, y}, {w, h}};
}

void Renderer::createGlobalSetLayout() {
#ifdef SAIDA_RHI_WEBGPU
    using WE = rhi::webgpu::BindGroupLayoutEntry;
    using Dim = rhi::webgpu::TextureDim;
    const auto FC = rhi::ShaderStages::Fragment | rhi::ShaderStages::Compute;
    const auto V = rhi::ShaderStages::Vertex;
    std::vector<WE> entries;
    WE e{};
    e.binding = 0; e.type = rhi::BindingType::UniformBuffer; e.visibility = V;
    entries.push_back(e);
    e = {}; e.binding = 1; e.type = rhi::BindingType::UniformBuffer; e.visibility = FC;
    entries.push_back(e);
    e = {}; e.binding = 2; e.type = rhi::BindingType::SampledTexture; e.visibility = FC;
    e.dim = Dim::Dim2DArray; e.depthTexture = true;
    entries.push_back(e);
    e = {}; e.binding = 3; e.type = rhi::BindingType::StorageBuffer; e.visibility = V;
    e.readOnlyStorage = true;
    entries.push_back(e);
    e = {}; e.binding = 4; e.type = rhi::BindingType::SampledTexture; e.visibility = FC;
    entries.push_back(e);
    e = {}; e.binding = 5; e.type = rhi::BindingType::SampledTexture; e.visibility = FC;
    entries.push_back(e);
    e = {}; e.binding = 6; e.type = rhi::BindingType::SampledTexture; e.visibility = FC;
    e.dim = Dim::Dim3D;
    entries.push_back(e);
    e = {}; e.binding = 7; e.type = rhi::BindingType::SampledTexture; e.visibility = FC;
    entries.push_back(e);
    e = {}; e.binding = 8; e.type = rhi::BindingType::Sampler; e.visibility = FC;
    e.comparisonSampler = true;
    entries.push_back(e);
    for (uint32_t b : {9u, 10u, 11u, 12u}) {
        e = {}; e.binding = b; e.type = rhi::BindingType::Sampler; e.visibility = FC;
        entries.push_back(e);
    }
    globalSetLayout_ = std::make_unique<rhi::BindGroupLayout>(device_, entries);
#else
    globalSetLayout_ = std::make_unique<rhi::BindGroupLayout>(device_,
        std::vector<rhi::BindGroupLayoutEntry>{
            {0, rhi::BindingType::UniformBuffer, rhi::ShaderStages::Vertex},                       // camera
            {1, rhi::BindingType::UniformBuffer, rhi::ShaderStages::Fragment | rhi::ShaderStages::Compute},  // lighting
            {2, rhi::BindingType::CombinedImageSampler, rhi::ShaderStages::Fragment | rhi::ShaderStages::Compute},  // shadow array
            {3, rhi::BindingType::StorageBuffer, rhi::ShaderStages::Vertex},                        // bone matrices
            {4, rhi::BindingType::CombinedImageSampler, rhi::ShaderStages::Fragment | rhi::ShaderStages::Compute},  // GI irradiance
            {5, rhi::BindingType::CombinedImageSampler, rhi::ShaderStages::Fragment | rhi::ShaderStages::Compute},  // GI visibility
            {6, rhi::BindingType::CombinedImageSampler, rhi::ShaderStages::Fragment | rhi::ShaderStages::Compute},  // GI voxel
            {7, rhi::BindingType::CombinedImageSampler, rhi::ShaderStages::Fragment | rhi::ShaderStages::Compute},  // environment
        });
#endif
}

void Renderer::createPipeline(rhi::BindGroupLayout& materialSetLayout) {
    rhi::Pipeline::Desc classic;
    classic.vertPath = shaderPath("shader.vert.spv");
    classic.fragPath = shaderPath("shader.frag.spv");
    classic.colorFormats = {rhi::Format::RGBA16Float};
    // WebGPU needs an explicit size to emit its group-3 push-constant emulation.
    classic.pushConstantSize = sizeof(PushConstants);
#ifdef SAIDA_RHI_WEBGPU
    classic.depthFormat = swapchain_->depthFormat();
    classic.samples = swapchain_->samples();
#else
    classic.depthFormat = rhi::vulkan::fromVk(swapchain_->depthFormat());
    classic.samples = sampleCountValue(swapchain_->samples());
#endif
    classic.bindGroupLayouts = {globalSetLayout_.get(), &materialSetLayout};
    pipeline_ = std::make_unique<rhi::Pipeline>(device_, classic);

    rhi::Pipeline::Desc unlit = classic;
    unlit.fragPath = shaderPath("unlit.frag.spv");
    unlitPipeline_ = std::make_unique<rhi::Pipeline>(device_, unlit);

#ifndef SAIDA_RHI_WEBGPU
    if (gpuDrivenAvailable_) {
        rhi::Pipeline::Desc gpuDriven = classic;
        gpuDriven.vertPath = shaderPath("bindless.shader.vert.spv");
        gpuDriven.fragPath = shaderPath("bindless.shader.frag.spv");
        gpuDriven.bindGroupLayouts = {globalSetLayout_.get(), resources_.globalMaterialSetLayout(),
                                      cullingSetLayout_.get()};
        gpuDrivenPipeline_ = std::make_unique<rhi::Pipeline>(device_, gpuDriven);
    }
#endif
}

void Renderer::createWebCanvasWorldPipeline() {
#ifdef SAIDA_RHI_WEBGPU
    return;
#else
    if (!resources_.globalMaterialSetLayout()) {
        Log::warn("WebCanvas world-space rendering disabled: descriptor indexing is unavailable.");
        return;
    }

    rhi::Pipeline::Desc desc;
    desc.vertPath = shaderPath("web_canvas_world.vert.spv");
    desc.fragPath = shaderPath("web_canvas_world.frag.spv");
    desc.colorFormats = {rhi::Format::RGBA16Float};
    desc.depthFormat = rhi::vulkan::fromVk(swapchain_->depthFormat());
    desc.bindGroupLayouts = {resources_.globalMaterialSetLayout()};  // raw bindless set
    desc.samples = sampleCountValue(swapchain_->samples());
    desc.vertexInput = false;
    desc.depthWrite = false;
    desc.depthCompare = rhi::CompareOp::LessOrEqual;
    desc.cullMode = rhi::CullMode::None;
    desc.blendMode = rhi::BlendMode::Alpha;
    desc.topology = rhi::Topology::TriangleStrip;
    desc.pushConstantSize = sizeof(WebCanvasWorldPushConstants);
    webCanvasWorldPipeline_ = std::make_unique<rhi::Pipeline>(device_, desc);
#endif
}

void Renderer::createUniformBuffers() {
    uniformBuffers_.reserve(kMaxFramesInFlight);
    lightingBuffers_.reserve(kMaxFramesInFlight);
    boneMatricesBuffers_.reserve(kMaxFramesInFlight);
    const uint64_t kBoneBufferSize = uint64_t(kMaxPaletteBones) * kBytesPerBone;
    for (int i = 0; i < kMaxFramesInFlight; i++) {
        uniformBuffers_.push_back(std::make_unique<rhi::Buffer>(device_, sizeof(UniformBufferObject),
            rhi::BufferUsage::Uniform, MemoryUsage::HostVisible));
        lightingBuffers_.push_back(std::make_unique<rhi::Buffer>(device_, sizeof(LightingUBO),
            rhi::BufferUsage::Uniform, MemoryUsage::HostVisible));
        boneMatricesBuffers_.push_back(std::make_unique<rhi::Buffer>(device_, kBoneBufferSize,
            rhi::BufferUsage::Storage, MemoryUsage::HostVisible));
    }
}

void Renderer::createGpuDrivenBuffers() {
    instanceBuffers_.reserve(kMaxFramesInFlight);
    originalDrawCommandBuffers_.reserve(kMaxFramesInFlight);
    drawCommandBuffers_.reserve(kMaxFramesInFlight);
    countBuffers_.reserve(kMaxFramesInFlight);

    uint64_t instanceBufferSize = kMaxInstances * sizeof(gpu_driven::InstanceData);
    uint64_t drawCommandBufferSize = kMaxInstances * sizeof(gpu_driven::DrawIndexedIndirectCommand);

    for (int i = 0; i < kMaxFramesInFlight; i++) {
        instanceBuffers_.push_back(std::make_unique<rhi::Buffer>(device_, instanceBufferSize,
            rhi::BufferUsage::Storage, MemoryUsage::HostVisible));
            
        originalDrawCommandBuffers_.push_back(std::make_unique<rhi::Buffer>(device_, drawCommandBufferSize,
            rhi::BufferUsage::Storage, MemoryUsage::HostVisible));
        
        drawCommandBuffers_.push_back(std::make_unique<rhi::Buffer>(device_, drawCommandBufferSize,
            rhi::BufferUsage::Storage | rhi::BufferUsage::Indirect,
            MemoryUsage::GpuOnly));
            
        countBuffers_.push_back(std::make_unique<rhi::Buffer>(device_, sizeof(uint32_t),
            rhi::BufferUsage::Storage | rhi::BufferUsage::Indirect | rhi::BufferUsage::TransferDst,
            MemoryUsage::GpuOnly));
    }
}

void Renderer::createCullingPipeline() {
#ifdef SAIDA_RHI_WEBGPU
    return;
#else
    cullingSetLayout_ = std::make_unique<rhi::BindGroupLayout>(device_,
        std::vector<rhi::BindGroupLayoutEntry>{
            {gpu_driven::binding(gpu_driven::CullingBinding::Instances),
             rhi::BindingType::StorageBuffer, rhi::ShaderStages::Compute | rhi::ShaderStages::Vertex},
            {gpu_driven::binding(gpu_driven::CullingBinding::OriginalDrawCommands),
             rhi::BindingType::StorageBuffer, rhi::ShaderStages::Compute},
            {gpu_driven::binding(gpu_driven::CullingBinding::DrawCount),
             rhi::BindingType::StorageBuffer, rhi::ShaderStages::Compute},
            {gpu_driven::binding(gpu_driven::CullingBinding::CulledDrawCommands),
             rhi::BindingType::StorageBuffer, rhi::ShaderStages::Compute},
        });

    cullingGroups_.resize(kMaxFramesInFlight);
    for (int i = 0; i < kMaxFramesInFlight; i++) {
        rhi::BindGroupEntry instances{gpu_driven::binding(gpu_driven::CullingBinding::Instances),
                                       instanceBuffers_[i].get()};
        rhi::BindGroupEntry originalDraws{
            gpu_driven::binding(gpu_driven::CullingBinding::OriginalDrawCommands),
            originalDrawCommandBuffers_[i].get()};
        rhi::BindGroupEntry count{gpu_driven::binding(gpu_driven::CullingBinding::DrawCount),
                                   countBuffers_[i].get()};
        rhi::BindGroupEntry culledDraws{
            gpu_driven::binding(gpu_driven::CullingBinding::CulledDrawCommands),
            drawCommandBuffers_[i].get()};
        cullingGroups_[i] = std::make_unique<rhi::BindGroup>(*cullingSetLayout_,
            std::vector<rhi::BindGroupEntry>{instances, originalDraws, count, culledDraws});
    }

    std::vector<rhi::vulkan::BindGroupLayoutRef> plLayouts = {*cullingSetLayout_};
    cullingPipeline_ = std::make_unique<ComputePipeline>(device_, shaderPath("culling.comp.spv"),
        plLayouts, sizeof(gpu_driven::CullingPushConstants));
#endif
}

void Renderer::rebuildGlobalSet(int frame) {
    rhi::BindGroupEntry cameraEntry;
    cameraEntry.binding = 0;
    cameraEntry.buffer = uniformBuffers_[frame].get();
    cameraEntry.range = sizeof(UniformBufferObject);

    rhi::BindGroupEntry lightEntry;
    lightEntry.binding = 1;
    lightEntry.buffer = lightingBuffers_[frame].get();
    lightEntry.range = sizeof(LightingUBO);

    rhi::BindGroupEntry shadowEntry;
    shadowEntry.binding = 2;
    shadowEntry.view = shadowMap_->arrayView();
#ifndef SAIDA_RHI_WEBGPU
    shadowEntry.sampler = shadowMap_->sampler();
    shadowEntry.textureState = rhi::ResourceState::DepthRead;
#endif

    rhi::BindGroupEntry boneEntry;
    boneEntry.binding = 3;
    boneEntry.buffer = boneMatricesBuffers_[frame].get();
    boneEntry.range = uint64_t(kMaxPaletteBones) * kBytesPerBone;

    rhi::BindGroupEntry giIrradianceEntry;
    giIrradianceEntry.binding = 4;
    giIrradianceEntry.view = gi_->irradianceView();
#ifndef SAIDA_RHI_WEBGPU
    giIrradianceEntry.sampler = gi_->sampler();
    giIrradianceEntry.textureState = rhi::ResourceState::StorageReadWrite;
#endif

    rhi::BindGroupEntry giVisibilityEntry;
    giVisibilityEntry.binding = 5;
    giVisibilityEntry.view = gi_->visibilityView();
#ifndef SAIDA_RHI_WEBGPU
    giVisibilityEntry.sampler = gi_->sampler();
    giVisibilityEntry.textureState = rhi::ResourceState::StorageReadWrite;
#endif

    rhi::BindGroupEntry giVoxelEntry;
    giVoxelEntry.binding = 6;
    giVoxelEntry.view = gi_->voxelView();
#ifndef SAIDA_RHI_WEBGPU
    giVoxelEntry.sampler = gi_->sampler();
#endif

    Texture* environment = resources_.defaultWhiteTexture();
    rhi::BindGroupEntry environmentEntry;
    environmentEntry.binding = 7;
    environmentEntry.view = environment->imageView();
#ifndef SAIDA_RHI_WEBGPU
    environmentEntry.sampler = environment->sampler();
#endif

#ifdef SAIDA_RHI_WEBGPU
    rhi::BindGroupEntry shadowSamplerEntry;
    shadowSamplerEntry.binding = 8;
    shadowSamplerEntry.sampler = shadowMap_->sampler();

    rhi::BindGroupEntry giIrradianceSamplerEntry;
    giIrradianceSamplerEntry.binding = 9;
    giIrradianceSamplerEntry.sampler = gi_->sampler();

    rhi::BindGroupEntry giVisibilitySamplerEntry;
    giVisibilitySamplerEntry.binding = 10;
    giVisibilitySamplerEntry.sampler = gi_->sampler();

    rhi::BindGroupEntry giVoxelSamplerEntry;
    giVoxelSamplerEntry.binding = 11;
    giVoxelSamplerEntry.sampler = gi_->sampler();

    rhi::BindGroupEntry environmentSamplerEntry;
    environmentSamplerEntry.binding = 12;
    environmentSamplerEntry.sampler = environment->sampler();

    globalGroups_[frame] = std::make_unique<rhi::BindGroup>(*globalSetLayout_,
        std::vector<rhi::BindGroupEntry>{cameraEntry, lightEntry, shadowEntry, boneEntry,
            giIrradianceEntry, giVisibilityEntry, giVoxelEntry, environmentEntry,
            shadowSamplerEntry, giIrradianceSamplerEntry, giVisibilitySamplerEntry,
            giVoxelSamplerEntry, environmentSamplerEntry});

    // Variant for the DDGI compute pass. WebGPU merges every bound group's
    // resources into the dispatch usage scope, so having the current atlases
    // sampled here (bindings 4/5) while the GI set storage-writes them fails
    // validation. The compute shaders never sample them — bind dummies.
    rhi::BindGroupEntry giIrradianceDummy = giIrradianceEntry;
    giIrradianceDummy.view = environment->imageView();
    rhi::BindGroupEntry giVisibilityDummy = giVisibilityEntry;
    giVisibilityDummy.view = environment->imageView();
    giComputeGlobalGroups_[frame] = std::make_unique<rhi::BindGroup>(*globalSetLayout_,
        std::vector<rhi::BindGroupEntry>{cameraEntry, lightEntry, shadowEntry, boneEntry,
            giIrradianceDummy, giVisibilityDummy, giVoxelEntry, environmentEntry,
            shadowSamplerEntry, giIrradianceSamplerEntry, giVisibilitySamplerEntry,
            giVoxelSamplerEntry, environmentSamplerEntry});
#else
    globalGroups_[frame] = std::make_unique<rhi::BindGroup>(*globalSetLayout_,
        std::vector<rhi::BindGroupEntry>{cameraEntry, lightEntry, shadowEntry, boneEntry,
            giIrradianceEntry, giVisibilityEntry, giVoxelEntry, environmentEntry});
#endif

    cachedGiIrradianceView_[frame] = gi_->irradianceView();
    cachedGiVisibilityView_[frame] = gi_->visibilityView();
    cachedGiSampler_[frame] = gi_->sampler();
    cachedEnvironmentView_[frame] = environment->imageView();
    cachedEnvironmentSampler_[frame] = environment->sampler();
}

void Renderer::createGlobalDescriptorSets() {
    globalGroups_.resize(kMaxFramesInFlight);
#ifdef SAIDA_RHI_WEBGPU
    giComputeGlobalGroups_.resize(kMaxFramesInFlight);
#endif
    for (int i = 0; i < kMaxFramesInFlight; i++) rebuildGlobalSet(i);
}

void Renderer::updateGlobalShadowDescriptor() {
    // The shadow map array view changes on resize; every frame slot samples the
    // same array, so every slot's bind group is stale and must be rebuilt.
    for (int i = 0; i < kMaxFramesInFlight; i++) rebuildGlobalSet(i);
}

void Renderer::updateGIDescriptors() {
    // After GIVolume::beginFrame() the "current" atlas (sampled by the lighting
    // pass) can change; rebuild this frame slot's bind group only when stale.
    // Safe: drawFrame already waited on this frame's fence.
    if (cachedGiIrradianceView_[currentFrame_] == gi_->irradianceView() &&
        cachedGiVisibilityView_[currentFrame_] == gi_->visibilityView() &&
        cachedGiSampler_[currentFrame_] == gi_->sampler()) {
        return;
    }
    rebuildGlobalSet(currentFrame_);
}

void Renderer::updateEnvironmentDescriptor(Scene& scene) {
    Texture* environment = resources_.defaultWhiteTexture();
    if (scene.settings().skyboxTexture != kAssetInvalid) {
        if (Texture* skybox = resources_.getTexture(scene.settings().skyboxTexture)) {
            environment = skybox;
        }
    }

    if (cachedEnvironmentView_[currentFrame_] == environment->imageView() &&
        cachedEnvironmentSampler_[currentFrame_] == environment->sampler()) {
        return;
    }
    rebuildGlobalSet(currentFrame_);
}

bool Renderer::shouldUpdateRealtimeGI(bool dirty) const {
    int cadence = 16;
    switch (device_.capabilities().tier) {
        case QualityTier::Ultra:
        case QualityTier::High:
            cadence = 8;
            break;
        case QualityTier::Medium:
            cadence = 12;
            break;
        case QualityTier::Low:
        default:
            cadence = 16;
            break;
    }

    if (giRealtimeWarmupRemaining_ > 0) return true;
    if (!dirty) return false;
    if (giLastMode_ == static_cast<int>(GIMode::FullRealtime)) return true;
    return (giFrameCounter_ % static_cast<uint64_t>(cadence)) == 0;
}

uint64_t Renderer::giDirtySignature(const Scene& scene) const {
    const SceneSettings& settings = scene.settings();
    uint64_t h = 1469598103934665603ull;
    hashCombine(h, static_cast<uint64_t>(settings.lightingMode));
    hashCombine(h, settings.giEnabled ? 1ull : 0ull);
    hashCombine(h, static_cast<uint64_t>(settings.giMode));
    hashFloat(h, settings.giIntensity);
    hashCombine(h, static_cast<uint64_t>(settings.skyboxTexture));
    hashFloat(h, settings.skyboxExposure);
    hashFloat(h, settings.skyboxRotation);
    hashCombine(h, settings.iblEnabled ? 1ull : 0ull);
    hashFloat(h, settings.iblDiffuseIntensity);
    hashFloat(h, settings.iblSpecularIntensity);

    hashCombine(h, static_cast<uint64_t>(scene.meshes().size()));
    hashCombine(h, static_cast<uint64_t>(scene.lights().size()));
    for (const LightNode* light : scene.lights()) {
        if (!light) continue;
        hashCombine(h, static_cast<uint64_t>(light->type));
        hashVec4(h, glm::vec4(light->color, light->intensity));
        hashFloat(h, light->range);
        hashFloat(h, light->spotInnerAngle);
        hashFloat(h, light->spotOuterAngle);
        hashCombine(h, light->castShadows ? 1ull : 0ull);
        hashMat4(h, light->worldTransform());
    }
    return h;
}

Renderer::TonemapPushConstants Renderer::tonemapPushConstants(
    const SceneSettings& settings, const glm::mat4& projection) const {
    TonemapPushConstants push{};
    push.invProjection = glm::inverse(projection);
    push.aoParams = glm::vec4(settings.aoEnabled ? 1.0f : 0.0f,
                              std::max(settings.aoRadius, 0.0f),
                              std::max(settings.aoIntensity, 0.0f),
                              std::max(settings.aoPower, kMinAoPower));
    push.fogColor = settings.fogColor;
    push.fogParams = glm::vec4(settings.fogEnabled ? 1.0f : 0.0f,
                               std::max(settings.fogStart, 0.0f),
                               std::max(settings.fogDensity, 0.0f), exposure_);
    push.bloomParams = glm::vec4(settings.bloomEnabled ? 1.0f : 0.0f,
                                 std::max(settings.bloomThreshold, 0.0f),
                                 std::max(settings.bloomIntensity, 0.0f),
                                 std::max(settings.bloomRadius, 0.0f));
    push.sourceRect = glm::vec4(0.0f, 0.0f, 1.0f, 1.0f);
    push.projectionParams = glm::vec4(push.invProjection[0][0],
                                      push.invProjection[1][1],
                                      push.invProjection[2][3],
                                      push.invProjection[3][3]);
    push.projectionParams2 = glm::vec4(push.invProjection[2][2],
                                       push.invProjection[3][2],
                                       0.0f, 0.0f);
    return push;
}

void Renderer::gatherScene(LightingUBO& ubo, Scene& scene, const glm::vec3& cameraPos,
                           const Frustum* cullFrustum, Project* project,
                           const glm::mat4* view, const glm::mat4* proj) {
    SAIDA_PROFILE_FUNCTION();
    lodMatricesValid_ = view && proj;
    if (lodMatricesValid_) {
        lodView_ = *view;
        lodProj_ = *proj;
    }
    auto& settings = scene.settings();
    ubo.ambient = settings.ambientLight;
    ubo.cameraPos = glm::vec4(cameraPos, 1.0f);
    ubo.shadowParams = glm::vec4(project ? project->shadowSoftness() : 1.0f, 0.0f, 0.0f, 0.0f);

    int lightCount = 0;
    shadowCount_ = 0;

    for (LightNode* light : scene.lights()) {
        if (lightCount >= kMaxLights) break;
        const glm::mat4& world = light->worldTransform();
        glm::vec3 worldPos = glm::vec3(world[3]);
        glm::vec3 worldDir = glm::normalize(glm::mat3(world) * light->direction);

        GpuLight& gl = ubo.lights[lightCount];
        gl.posRange = glm::vec4(worldPos, light->range);
        gl.colorInt = glm::vec4(light->color, light->intensity);
        float type = light->type == LightType::Directional ? 0.0f
                   : light->type == LightType::Point       ? 1.0f
                                                           : 2.0f;  // Spot
        gl.dirType = glm::vec4(worldDir, type);
        gl.spotShadow = glm::vec4(0.0f, 0.0f, -1.0f, 0.0f);

        if (light->type == LightType::Spot) {
            gl.spotShadow.x = std::cos(glm::radians(light->spotInnerAngle));
            gl.spotShadow.y = std::cos(glm::radians(light->spotOuterAngle));
        }

    bool wantsShadow = shadowsEnabled_ && light->castShadows &&
            (light->type == LightType::Directional || light->type == LightType::Spot);
        if (wantsShadow && shadowCount_ < kMaxShadowCasters) {
            glm::mat4 lightVP = light->type == LightType::Directional
                ? directionalMatrix(
                      worldDir, project ? project->shadowDistance()
                                        : kDefaultShadowDistance)
                : spotMatrix(worldPos, worldDir, light->spotOuterAngle, light->range);
            shadowMatrices_[shadowCount_] = lightVP;
            ubo.shadowMatrices[shadowCount_] = lightVP;
            gl.spotShadow.z = static_cast<float>(shadowCount_);
            ++shadowCount_;
        }

        ++lightCount;
    }

    int mode = settings.lightingMode == LightingMode::Baked ? 1 : 0;
    ubo.counts = glm::ivec4(lightCount, mode, 0, 0);

    const GIVolumeDesc& gd = gi_->desc();
    ubo.giOrigin  = glm::vec4(gd.origin, settings.giEnabled ? 1.0f : 0.0f);
    ubo.giSpacing = glm::vec4(gd.spacing, settings.giIntensity);  // w = indirect multiplier
    ubo.giCounts  = glm::ivec4(gd.counts, gi_->probesPerRow());
    ubo.giAtlas   = glm::ivec4(gd.irradianceTexels, gd.visibilityTexels,
                               settings.giDebugVoxels ? 1 : 0, gd.voxelResolution);
    const bool iblActive = settings.iblEnabled && settings.skyboxTexture != kAssetInvalid;
    const float environmentExposure = std::max(settings.skyboxExposure, 0.0f);
    ubo.environmentParams = glm::vec4(
        iblActive ? 1.0f : 0.0f,
        std::max(settings.iblDiffuseIntensity, 0.0f) * environmentExposure,
        std::max(settings.iblSpecularIntensity, 0.0f) * environmentExposure,
        settings.skyboxRotation);

    auto getAnimatorInParent = [](Node* n) -> Animator* {
        while (n) {
            if (auto* a = n->getBehaviour<Animator>()) return a;
            n = n->parent();
        }
        return nullptr;
    };

    uint32_t currentBoneCount = 0;
    glm::vec4* boneRows = static_cast<glm::vec4*>(boneMatricesBuffers_[currentFrame_]->mapped());
    std::unordered_map<Animator*, int32_t> animatorBoneOffsets;

    // Share one palette slice per Animator and reject overflow. The palette is
    // affine 3x4: three vec4 rows per bone (the transposed top of the mat4).
    auto boneOffsetFor = [&](MeshNode* node) -> int32_t {
        Animator* animator = getAnimatorInParent(node);
        if (!animator || animator->globalPose().skinningMatrices.empty()) return -1;

        auto found = animatorBoneOffsets.find(animator);
        if (found != animatorBoneOffsets.end()) return found->second;

        const auto& matrices = animator->globalPose().skinningMatrices;
        const uint32_t boneCount = static_cast<uint32_t>(matrices.size());
        if (boneCount > kMaxPaletteBones || currentBoneCount > kMaxPaletteBones - boneCount) {
            Log::warn("Renderer: bone palette exhausted; rendering one skeleton unskinned this frame.");
            animatorBoneOffsets.emplace(animator, -1);
            return -1;
        }

        const int32_t offset = static_cast<int32_t>(currentBoneCount);
        glm::vec4* rows = boneRows + size_t(currentBoneCount) * kBoneRowsPerBone;
        for (uint32_t b = 0; b < boneCount; ++b) {
            const glm::mat4& m = matrices[b];
            for (uint32_t r = 0; r < kBoneRowsPerBone; ++r)
                rows[b * kBoneRowsPerBone + r] = glm::vec4(m[0][r], m[1][r], m[2][r], m[3][r]);
        }
        currentBoneCount += boneCount;
        animatorBoneOffsets.emplace(animator, offset);
        return offset;
    };

    shadowDraws_.clear();
    if (shadowCount_ > 0) {
        for (MeshNode* node : scene.meshes()) {
            if (!node->castShadows()) continue;
            Mesh* mesh = node->mesh();
            if (!mesh) continue;
            const glm::mat4& world = node->worldTransform();
            if (node->hasLods() && lodMatricesValid_) {
                const float coverage = computeScreenCoverage(world, mesh->bounds(), lodView_, lodProj_);
                mesh = node->meshForLod(node->selectLodIndex(coverage));
            }
            if (mesh) shadowDraws_.push_back(ShadowDraw{mesh, world});
        }
    }

    frameDraws_.gpuCandidates.clear();
    frameDraws_.visibleDraws.clear();
    gpuFrameActive_ = gpuDrivenAvailable_;
    currentInstanceCount_ = 0;

    {
        SAIDA_PROFILE_SCOPE("Renderer/PrepareDrawLists");

        for (MeshNode* node : scene.meshes()) {
            if (Mesh* m = node->mesh()) {
                if (Material* mat = node->material()) {
                    const glm::mat4& world = node->worldTransform();
                    Mesh* drawMesh = m;
                    Material* drawMat = mat;
                    if (node->hasLods() && lodMatricesValid_) {
                        const float coverage = computeScreenCoverage(world, m->bounds(), lodView_, lodProj_);
                        const int lod = node->selectLodIndex(coverage);
                        node->setActiveLodIndex(lod);
                        drawMesh = node->meshForLod(lod);
                        drawMat = node->materialForLod(lod);
                    }
                    if (!drawMesh || !drawMat) continue;

                    // Frustum culling against the camera (desktop). XR passes a
                    // null frustum (stereo) → render everything, no per-eye cull.
                    bool inside = true;
                    if (cullFrustum) {
                        float maxScale = std::max({
                            glm::length(glm::vec3(world[0])),
                            glm::length(glm::vec3(world[1])),
                            glm::length(glm::vec3(world[2]))
                        });
                        const float localRadius = glm::length(drawMesh->bounds().extent()) * 0.5f;
                        float radius =
                            (localRadius > kMinBoundsRadius
                                 ? localRadius
                                 : kUnitCubeBoundsRadius) *
                            maxScale;
                        glm::vec3 center = glm::vec3(world * glm::vec4(drawMesh->bounds().center(), 1.0f));
                        for (int i = 0; i < 6; ++i) {
                            if (glm::dot(glm::vec3(cullFrustum->planes[i]), center) +
                                    cullFrustum->planes[i].w < -radius) {
                                inside = false;
                                break;
                            }
                        }
                    }

    const int32_t boneOffset = (gpuFrameActive_ || inside) ? boneOffsetFor(node) : -1;
                    SceneDraw draw{drawMesh, drawMat, node, world, node->castShadows(),
                                   boneOffset, drawMat->desc().type};

                    if (gpuFrameActive_) {
                        if (frameDraws_.gpuCandidates.size() == kMaxInstances) {
                            gpuFrameActive_ = false;
                            frameDraws_.gpuCandidates.clear();
                            Log::warn("Renderer: GPU-driven instance capacity exceeded; using CPU submission for this frame.");
                        } else {
                            frameDraws_.gpuCandidates.push_back(draw);
                        }
                    }
                    if (inside) frameDraws_.visibleDraws.push_back(draw);
                }
            }
        }

        if (currentBoneCount > 0) {
            boneMatricesBuffers_[currentFrame_]->flush(uint64_t(currentBoneCount) * kBytesPerBone);
        }
        Profiler::instance().setCounter("anim/palette bones", double(currentBoneCount));
        Profiler::instance().setCounter("anim/palette bytes",
                                        double(uint64_t(currentBoneCount) * kBytesPerBone));

        if (gpuFrameActive_) {
            uploadGpuDrivenDraws();
        } else {
            std::sort(frameDraws_.visibleDraws.begin(), frameDraws_.visibleDraws.end(),
                      [](const SceneDraw& a, const SceneDraw& b) {
                          if (a.materialType != b.materialType) return a.materialType < b.materialType;
                          return a.material < b.material;
                      });
        }
    }

    SAIDA_PROFILE_COUNTER("Renderer/VisibleDraws", frameDraws_.visibleDraws.size());
    SAIDA_PROFILE_COUNTER("Renderer/SubmissionCandidates", frameDraws_.gpuCandidates.size());
    SAIDA_PROFILE_COUNTER("Renderer/GpuDrivenActive", gpuFrameActive_ ? 1 : 0);
    SAIDA_PROFILE_COUNTER("Renderer/ShadowCasters", shadowDraws_.size());
    SAIDA_PROFILE_COUNTER("Renderer/ShadowLights", shadowCount_);
    SAIDA_PROFILE_COUNTER("Animation/BoneMatrices", currentBoneCount);
}

const std::vector<SceneDraw>& Renderer::featureDraws() const {
    return frameDraws_.visibleDraws;
}

void Renderer::uploadGpuDrivenDraws() {
#ifdef SAIDA_RHI_WEBGPU
    // WebGPU currently uses the descriptor-per-material path. This function is
    // unreachable there, but keeping the no-op makes the frame representation
    // backend-neutral.
    gpuFrameActive_ = false;
#else
    const auto& candidates = frameDraws_.gpuCandidates;
    currentInstanceCount_ = static_cast<uint32_t>(candidates.size());
    if (currentInstanceCount_ == 0) return;

    auto* instances = static_cast<gpu_driven::InstanceData*>(
        instanceBuffers_[currentFrame_]->mapped());
    auto* commands = static_cast<gpu_driven::DrawIndexedIndirectCommand*>(
        originalDrawCommandBuffers_[currentFrame_]->mapped());

    for (uint32_t index = 0; index < currentInstanceCount_; ++index) {
        const SceneDraw& draw = candidates[index];
        const Aabb& bounds = draw.mesh->bounds();

        gpu_driven::InstanceData& instance = instances[index];
        instance.model = draw.world;
        instance.boundingSphere = glm::vec4(
            bounds.center(),
            std::max(glm::length(bounds.extent()) * 0.5f,
                     kMinBoundsRadius));
        instance.materialIndex = draw.material->bindlessIndex();
        instance.boneOffset = draw.boneOffset;

        const GeometryAllocation allocation = draw.mesh->geometryAllocation();
        gpu_driven::DrawIndexedIndirectCommand& command = commands[index];
        command.indexCount = allocation.indexCount;
        command.instanceCount = 1;
        command.firstIndex = allocation.firstIndex;
        command.vertexOffset = allocation.vertexOffset;
        command.firstInstance = index;
    }

    instanceBuffers_[currentFrame_]->flush(
        uint64_t(currentInstanceCount_) * sizeof(gpu_driven::InstanceData));
    originalDrawCommandBuffers_[currentFrame_]->flush(
        uint64_t(currentInstanceCount_) * sizeof(gpu_driven::DrawIndexedIndirectCommand));
#endif
}

void Renderer::createHdrResources() {
#ifdef SAIDA_RHI_WEBGPU
    rhi::Extent2D extent = swapchain_->extent();
    const bool msaa = false;
#else
    rhi::Extent2D extent = swapchain_->extent();
    const bool msaa = swapchain_->samples() != VK_SAMPLE_COUNT_1_BIT;
#endif

    rhi::RenderTextureDesc hdrDesc;
    hdrDesc.format = rhi::Format::RGBA16Float;
    hdrDesc.width = extent.width;
    hdrDesc.height = extent.height;
    hdrDesc.usage = rhi::TextureUsage::ColorAttachment | rhi::TextureUsage::Sampled;
    hdrDesc.memoryCategory = "RenderTarget/DesktopHDR";
    hdrTexture_ = std::make_unique<rhi::RenderTexture>(device_, hdrDesc);

    if (msaa) {
        rhi::RenderTextureDesc msaaDesc = hdrDesc;
#ifdef SAIDA_RHI_WEBGPU
        msaaDesc.samples = swapchain_->samples();
#else
        msaaDesc.samples = sampleCountValue(swapchain_->samples());
#endif
        msaaDesc.usage = rhi::TextureUsage::ColorAttachment | rhi::TextureUsage::Transient;
        hdrMsaaTexture_ = std::make_unique<rhi::RenderTexture>(device_, msaaDesc);

        rhi::RenderTextureDesc depthDesc;
#ifdef SAIDA_RHI_WEBGPU
        depthDesc.format = swapchain_->depthFormat();
#else
        depthDesc.format = rhi::vulkan::fromVk(swapchain_->depthFormat());
#endif
        depthDesc.width = extent.width;
        depthDesc.height = extent.height;
        depthDesc.usage = rhi::TextureUsage::DepthAttachment | rhi::TextureUsage::Sampled;
        depthDesc.memoryCategory = "RenderTarget/DesktopHDR";
        depthResolveTexture_ = std::make_unique<rhi::RenderTexture>(device_, depthDesc);
    }

    postProcessor_ = std::make_unique<PostProcessor>(device_, extent,
        rhi::Format::RGBA16Float, hdrTexture_->view());
    updateTonemapDescriptorSet();
}

void Renderer::cleanupHdrResources() {
    postProcessor_.reset();
    depthResolveTexture_.reset();
    hdrMsaaTexture_.reset();
    hdrTexture_.reset();
}

void Renderer::createTonemapPipeline() {
#ifdef SAIDA_RHI_WEBGPU
    using WE = rhi::webgpu::BindGroupLayoutEntry;
    const auto F = rhi::ShaderStages::Fragment;
    std::vector<WE> tonemapEntries;
    WE e{};
    e.binding = 0; e.type = rhi::BindingType::SampledTexture; e.visibility = F;
    tonemapEntries.push_back(e);
    e = {}; e.binding = 1; e.type = rhi::BindingType::SampledTexture; e.visibility = F;
    e.unfilterable = true;
    tonemapEntries.push_back(e);
    e = {}; e.binding = 2; e.type = rhi::BindingType::SampledTexture; e.visibility = F;
    tonemapEntries.push_back(e);
    e = {}; e.binding = 3; e.type = rhi::BindingType::Sampler; e.visibility = F;
    tonemapEntries.push_back(e);
    e = {}; e.binding = 4; e.type = rhi::BindingType::Sampler; e.visibility = F;
    e.nonFilteringSampler = true;
    tonemapEntries.push_back(e);
    e = {}; e.binding = 5; e.type = rhi::BindingType::Sampler; e.visibility = F;
    tonemapEntries.push_back(e);
    tonemapSetLayout_ = std::make_unique<rhi::BindGroupLayout>(device_, tonemapEntries);
#else
    tonemapSetLayout_ = std::make_unique<rhi::BindGroupLayout>(device_,
        std::vector<rhi::BindGroupLayoutEntry>{
            {0, rhi::BindingType::CombinedImageSampler, rhi::ShaderStages::Fragment},  // HDR
            {1, rhi::BindingType::CombinedImageSampler, rhi::ShaderStages::Fragment},  // depth (AO)
            {2, rhi::BindingType::CombinedImageSampler, rhi::ShaderStages::Fragment},  // bloom
        });
#endif

    rhi::SamplerDesc linearDesc;
    linearDesc.mipFilter = rhi::FilterMode::Linear;
    tonemapSampler_ = std::make_unique<rhi::Sampler>(device_, linearDesc);

    rhi::SamplerDesc nearestDesc;
    nearestDesc.magFilter = rhi::FilterMode::Nearest;
    nearestDesc.minFilter = rhi::FilterMode::Nearest;
    tonemapDepthSampler_ = std::make_unique<rhi::Sampler>(device_, nearestDesc);

    rhi::Pipeline::Desc tonemapDesc;
    tonemapDesc.vertPath = shaderPath("tonemap.vert.spv");
    tonemapDesc.fragPath = shaderPath("tonemap.frag.spv");
#ifdef SAIDA_RHI_WEBGPU
    tonemapDesc.colorFormats = {swapchain_->colorFormat()};
#else
    tonemapDesc.colorFormats = {rhi::vulkan::fromVk(swapchain_->colorFormat())};
#endif
    tonemapDesc.bindGroupLayouts = {tonemapSetLayout_.get()};
    tonemapDesc.vertexInput = false;
    tonemapDesc.depthTest = false;
    // Fullscreen triangle: never cull. On web the naga SPIR-V→WGSL pass flips
    // clip-space Y, which inverts the winding of raw-clip-coord triangles (the
    // scene is unaffected: its projection flip cancels naga's) — with the
    // default Back cull the tonemap triangle disappears entirely.
    tonemapDesc.cullMode = rhi::CullMode::None;
    tonemapDesc.pushConstantSize = sizeof(TonemapPushConstants);
    tonemapPipeline_ = std::make_unique<rhi::Pipeline>(device_, tonemapDesc);

    updateTonemapDescriptorSet();
}

void Renderer::updateTonemapDescriptorSet() {
    if (!tonemapSetLayout_ || !hdrTexture_ || !swapchain_ || !tonemapSampler_ || !tonemapDepthSampler_ || !postProcessor_) {
        return;
    }

    rhi::BindGroupEntry hdrEntry;
    hdrEntry.binding = 0;
    hdrEntry.view = hdrTexture_->view();
#ifndef SAIDA_RHI_WEBGPU
    hdrEntry.sampler = tonemapSampler_->handle();
#endif

    rhi::BindGroupEntry depthEntry;
    depthEntry.binding = 1;
    depthEntry.view = depthResolveTexture_ ? depthResolveTexture_->view() : swapchain_->depthView();
#ifndef SAIDA_RHI_WEBGPU
    depthEntry.sampler = tonemapDepthSampler_->handle();
#endif

    rhi::BindGroupEntry bloomEntry;
    bloomEntry.binding = 2;
    bloomEntry.view = postProcessor_->bloomView();
#ifndef SAIDA_RHI_WEBGPU
    bloomEntry.sampler = postProcessor_->bloomSampler();
#endif

#ifdef SAIDA_RHI_WEBGPU
    rhi::BindGroupEntry hdrSamplerEntry;
    hdrSamplerEntry.binding = 3;
    hdrSamplerEntry.sampler = tonemapSampler_->handle();

    rhi::BindGroupEntry depthSamplerEntry;
    depthSamplerEntry.binding = 4;
    depthSamplerEntry.sampler = tonemapDepthSampler_->handle();

    rhi::BindGroupEntry bloomSamplerEntry;
    bloomSamplerEntry.binding = 5;
    bloomSamplerEntry.sampler = postProcessor_->bloomSampler();

    tonemapSet_ = std::make_unique<rhi::BindGroup>(*tonemapSetLayout_,
        std::vector<rhi::BindGroupEntry>{hdrEntry, depthEntry, bloomEntry,
            hdrSamplerEntry, depthSamplerEntry, bloomSamplerEntry});
#else
    tonemapSet_ = std::make_unique<rhi::BindGroup>(*tonemapSetLayout_,
        std::vector<rhi::BindGroupEntry>{hdrEntry, depthEntry, bloomEntry});
#endif
}

void Renderer::recordTonemapPass(rhi::CommandEncoder& encoder, uint32_t imageIndex,
                                 Scene& scene, const Camera& camera) {
    SAIDA_PROFILE_FUNCTION();
#ifdef SAIDA_RHI_WEBGPU
    auto cmd = encoder.handle();
#else
    VkCommandBuffer cmd = encoder.handle();  // GPU profiler zones (desktop tooling)
#endif
    GpuProfiler* gpuProfiler = Profiler::instance().enabled() ? gpuProfiler_.get() : nullptr;
    SAIDA_GPU_PROFILE_SCOPE(gpuProfiler, cmd, "GPU/Tonemap+EditorUI");

    // Scene results -> sampled inputs; the swapchain image comes back from
    // presentation with undefined contents (fully overwritten by this pass).
    encoder.transition(hdrTexture_->image(), rhi::ResourceState::ColorAttachment,
                       rhi::ResourceState::ShaderRead);
    auto depthImage = depthResolveTexture_ ? depthResolveTexture_->image()
                                           : swapchain_->depthImage();
    encoder.transition(depthImage, rhi::ResourceState::DepthWrite,
                       rhi::ResourceState::ShaderRead);
    encoder.transition(swapchain_->image(imageIndex), rhi::ResourceState::ColorAttachment,
                       rhi::ResourceState::ColorAttachment, 0, kAllTextureLayers,
                       /*discardContents=*/true);

    rhi::Rect2D renderRect = activeRenderRect();
    rhi::Extent2D fullExtent = swapchain_->extent();
    glm::vec4 sourceRect(
        static_cast<float>(renderRect.offset.x) / static_cast<float>(std::max(1u, fullExtent.width)),
        static_cast<float>(renderRect.offset.y) / static_cast<float>(std::max(1u, fullExtent.height)),
        static_cast<float>(renderRect.extent.width) / static_cast<float>(std::max(1u, fullExtent.width)),
        static_cast<float>(renderRect.extent.height) / static_cast<float>(std::max(1u, fullExtent.height)));

    if (postProcessor_) {
        postProcessor_->recordBloom(encoder, scene.settings(), sourceRect, gpuProfiler);
    }

    rhi::RenderPassDesc pass;
    pass.colorCount = 1;
    pass.colors[0].view = swapchain_->imageView(imageIndex);
    pass.colors[0].loadOp = rhi::LoadOp::Clear;
    pass.width = fullExtent.width;
    pass.height = fullExtent.height;
    pass.defaultViewportScissor = false;  // tonemap draws into the viewport rect only

    rhi::RenderPassEncoder rp = encoder.beginRenderPass(pass);
    rp.setPipeline(*tonemapPipeline_);
    rp.setViewport(static_cast<float>(renderRect.offset.x), static_cast<float>(renderRect.offset.y),
                   static_cast<float>(renderRect.extent.width), static_cast<float>(renderRect.extent.height));
    rp.setScissor(renderRect.offset.x, renderRect.offset.y,
                  renderRect.extent.width, renderRect.extent.height);
    rp.setBindGroup(0, *tonemapSet_);

    TonemapPushConstants push = tonemapPushConstants(scene.settings(), camera.projection());
    push.sourceRect = sourceRect;
    {
        SAIDA_GPU_PROFILE_SCOPE(gpuProfiler, cmd, "Post/Tonemap");
        rp.setPushConstants(&push, sizeof(TonemapPushConstants));
        rp.draw(3);
    }
    {
        SAIDA_GPU_PROFILE_SCOPE(gpuProfiler, cmd, "Post/UI");
        {
            SAIDA_PROFILE_SCOPE("UI/RecordCommands");
            uiRenderer_->recordCommands(rp, fullExtent.width, fullExtent.height,
                                        {static_cast<float>(renderRect.offset.x), static_cast<float>(renderRect.offset.y)},
                                        {static_cast<float>(renderRect.extent.width), static_cast<float>(renderRect.extent.height)});
        }
#ifndef SAIDA_RHI_WEBGPU
        {
            SAIDA_PROFILE_SCOPE("ImGui/RenderDrawData");
            imgui_->renderDrawData(rp.handle());  // editor-only overlay, permanent escape hatch
        }
#endif
    }
    rp.end();
}

void Renderer::buildFeatures(uint32_t viewMask, rhi::Format depthFormat,
                            rhi::SampleCount samples) {
    // The Renderer knows ONLY the ScenePassFeature interface. Concrete effects
    // (water, skybox, particles, …) plug in through the registry, so this never
    // names or includes a concrete effect — adding one touches no Renderer code.
    // On web the registry only carries the ported subset (skybox + GPU
    // particles); WaterFeature/Outline/DebugLines stay desktop-only.
    features_ = RenderFeatureRegistry::instance().build();
    RenderContext ctx{device_, resources_, *globalSetLayout_,
                      rhi::Format::RGBA16Float, depthFormat, samples,
                      viewMask, static_cast<uint32_t>(kMaxFramesInFlight)};
    for (auto& f : features_) f->createPipelines(ctx);
}

void Renderer::recordFeatures(FrameContext& fc) {
    for (auto& f : features_) f->record(fc);
}

void Renderer::recordMeshDraws(rhi::RenderPassEncoder& rp, rhi::Pipeline* firstPipeline, bool xrMultiview) {
    SAIDA_PROFILE_FUNCTION();
    if (!firstPipeline) return;

    Material* lastMaterial = nullptr;
    rhi::Pipeline* activePipeline = firstPipeline;
    uint64_t drawCalls = 0;
    uint64_t triangles = 0;
    const rhi::BindGroup& globalSet = *globalGroups_[currentFrame_];
    rp.setPipeline(*activePipeline);
    rp.setBindGroup(0, globalSet);

    for (const auto& draw : frameDraws_.visibleDraws) {
        rhi::Pipeline* want = scenePipelineFor(draw.materialType);
#ifdef SAIDA_ENABLE_XR
        if (xrMultiview)
            want = xrScenePipelineFor(draw.materialType);
#else
        (void)xrMultiview;
#endif

        if (want != activePipeline) {
            rp.setPipeline(*want);
            rp.setBindGroup(0, globalSet);
            activePipeline = want;
            lastMaterial = nullptr;
        }

        if (draw.material != lastMaterial) {
            rp.setBindGroup(1, draw.material->descriptorSet());
            lastMaterial = draw.material;
        }

        PushConstants pc{};
        pc.model = draw.world;
        // params.y: offset into the global bone matrix buffer, or -1.
        pc.params = glm::vec4(0.0f, static_cast<float>(draw.boneOffset), 0.0f, 0.0f);
        rp.setPushConstants(&pc, sizeof(PushConstants));

        draw.mesh->bind(rp);
        draw.mesh->draw(rp);
        ++drawCalls;
        triangles += draw.mesh->allocation().indexCount / 3;
    }
    SAIDA_PROFILE_COUNTER("Renderer/DrawCalls", drawCalls);
    SAIDA_PROFILE_COUNTER("Renderer/Triangles", triangles);
}

void Renderer::recordWorldWebCanvases(rhi::RenderPassEncoder& rp, Scene& scene, const Camera& camera) {
#ifdef SAIDA_RHI_WEBGPU
    (void)rp;
    (void)scene;
    (void)camera;
#else
    if (!webCanvasWorldPipeline_ || !resources_.globalMaterialSet()) return;

    std::vector<WebCanvasWorldDraw> draws = collectWorldWebCanvasDraws(scene, camera.position);
    if (draws.empty()) return;

    rp.setPipeline(*webCanvasWorldPipeline_);
    rp.setBindGroup(0, resources_.globalMaterialSet());

    for (const WebCanvasWorldDraw& draw : draws) {
        WebCanvasNode* node = draw.node;
        Texture* texture = node->texture();
        if (!texture) continue;

        WebCanvasWorldPushConstants pc{};
        pc.model = node->worldTransform();
        pc.params = glm::vec4(node->worldWidth(), node->worldHeight(),
                              static_cast<float>(resources_.ensureBindlessTextureIndex(texture)), 1.0f);
        rp.setPushConstants(&pc, sizeof(pc));
        rp.draw(4);
    }
#endif
}

void Renderer::recordShadowPasses(rhi::CommandEncoder& encoder) {
    shadowMap_->record(encoder, shadowCount_,
        [this](rhi::RenderPassEncoder& rp, int layer) {
            for (const ShadowDraw& draw : shadowDraws_) {
                glm::mat4 mvp = shadowMatrices_[layer] * draw.world;
                rp.setPushConstants(&mvp, sizeof(mvp));
                draw.mesh->bind(rp);
                draw.mesh->draw(rp);
            }
        });
}

void Renderer::updateUniformBuffer(uint32_t frame, Scene& scene, Camera& camera, Project* project) {
    rhi::Rect2D renderRect = activeRenderRect();
    float aspect = renderRect.extent.width / static_cast<float>(std::max(1u, renderRect.extent.height));
    camera.setPerspective(glm::radians(camera.fovDegrees), aspect,
                          camera.nearZ, camera.farZ);
    
    cameraFrustum_ = camera.getFrustum();

    UniformBufferObject ubo{};
    ubo.view[0] = ubo.view[1] = camera.view();
    ubo.proj[0] = ubo.proj[1] = camera.projection();
    uniformBuffers_[frame]->write(&ubo, sizeof(ubo));

    LightingUBO lighting{};
    gatherScene(lighting, scene, camera.position, &cameraFrustum_, project,
                &ubo.view[0], &ubo.proj[0]);
    lightingBuffers_[frame]->write(&lighting, sizeof(lighting));
}

void Renderer::recordCommandBuffer(rhi::CommandEncoder& encoder, uint32_t imageIndex,
                                   Scene& scene, const Camera& camera) {
#ifdef SAIDA_RHI_WEBGPU
    auto cmd = encoder.handle();
#else
    VkCommandBuffer cmd = encoder.handle();
#endif
    GpuProfiler* gpuProfiler = Profiler::instance().enabled() ? gpuProfiler_.get() : nullptr;
    if (gpuProfiler) gpuProfiler->resetQueries(cmd);
    uint32_t gpuFrameZone = gpuProfiler ? gpuProfiler->beginZone(cmd, "GPU/Frame") : UINT32_MAX;

    {
        SAIDA_PROFILE_SCOPE("UI/UpdateAsyncTextures");
        uiRenderer_->updateAsyncTextures(encoder);
    }

    if (giUpdateThisFrame_) {
        SAIDA_GPU_PROFILE_SCOPE(gpuProfiler, cmd, "GPU/DDGI");
        gi_->voxelize(encoder, scene, gpuProfiler);
#ifdef SAIDA_RHI_WEBGPU
        gi_->update(encoder, *giComputeGlobalGroups_[currentFrame_], gpuProfiler);
#else
        gi_->update(encoder, *globalGroups_[currentFrame_], gpuProfiler);
#endif
    }

    if (gpuFrameActive_ && currentInstanceCount_ > 0) {
        encoder.fillBuffer(*countBuffers_[currentFrame_], 0, sizeof(uint32_t), 0);
        encoder.transferToComputeBarrier();

        rhi::ComputePassEncoder cp = encoder.beginComputePass();
        cp.setPipeline(*cullingPipeline_);
        cp.setBindGroup(0, *cullingGroups_[currentFrame_]);

        gpu_driven::CullingPushConstants pc{};
        for (int i = 0; i < 6; ++i) pc.frustumPlanes[i] = cameraFrustum_.planes[i];
        pc.instanceCount = currentInstanceCount_;
        cp.setPushConstants(&pc, sizeof(pc));
        cp.dispatch((currentInstanceCount_ + 63) / 64);
        cp.end();

        encoder.computeToIndirectBarrier();
    }

    {
        SAIDA_GPU_PROFILE_SCOPE(gpuProfiler, cmd, "GPU/Shadows");
        recordShadowPasses(encoder);
    }

    auto& settings = scene.settings();
    glm::vec4 clearColor = settings.clearColor;
    rhi::Rect2D renderRect = activeRenderRect();
    rhi::Extent2D extent = renderRect.extent;

    // Compute features must run outside the scene render pass.
    {
        SAIDA_GPU_PROFILE_SCOPE(gpuProfiler, cmd, "Scene/FeaturesPrePass");
        PrePassContext ppc{encoder, currentFrame_, scene, Time::elapsed(), false,
                           &camera, nullptr, extent};
        for (auto& f : features_) f->recordPrePass(ppc);
    }

#ifdef SAIDA_RHI_WEBGPU
    const bool msaa = false;
#else
    const bool msaa = swapchain_->samples() != VK_SAMPLE_COUNT_1_BIT;
#endif
    auto colorImage = msaa ? hdrMsaaTexture_->image() : hdrTexture_->image();
    auto colorView = msaa ? hdrMsaaTexture_->view() : hdrTexture_->view();

    // Discard preserves synchronization while dropping contents overwritten by the pass.
    encoder.transition(colorImage, rhi::ResourceState::ColorAttachment,
                       rhi::ResourceState::ColorAttachment, 0, kAllTextureLayers,
                       /*discardContents=*/true);
    if (msaa) {
        encoder.transition(hdrTexture_->image(), rhi::ResourceState::ColorAttachment,
                           rhi::ResourceState::ColorAttachment, 0, kAllTextureLayers,
                           /*discardContents=*/true);
        encoder.transition(depthResolveTexture_->image(), rhi::ResourceState::DepthWrite,
                           rhi::ResourceState::DepthWrite, 0, kAllTextureLayers,
                           /*discardContents=*/true);
    }
    encoder.transition(swapchain_->depthImage(), rhi::ResourceState::DepthWrite,
                       rhi::ResourceState::DepthWrite, 0, kAllTextureLayers,
                       /*discardContents=*/true);

    rhi::RenderPassDesc scenePass;
    scenePass.colorCount = 1;
    scenePass.colors[0].view = colorView;
    scenePass.colors[0].loadOp = rhi::LoadOp::Clear;
    scenePass.colors[0].clearColor = {{clearColor.r, clearColor.g, clearColor.b, clearColor.a}};
    scenePass.colors[0].store = !msaa;                          // MSAA source is resolved, not kept
    scenePass.colors[0].resolveView = msaa ? hdrTexture_->view() : rhi::TextureView{};
    scenePass.depth.view = swapchain_->depthView();
    scenePass.depth.loadOp = rhi::LoadOp::Clear;
    scenePass.depth.store = !msaa;
    scenePass.depth.resolveView = msaa ? depthResolveTexture_->view() : rhi::TextureView{};
    scenePass.x = renderRect.offset.x;
    scenePass.y = renderRect.offset.y;
    scenePass.width = extent.width;
    scenePass.height = extent.height;

    {
    SAIDA_GPU_PROFILE_SCOPE(gpuProfiler, cmd, "GPU/SceneHDR");
    rhi::RenderPassEncoder rp = encoder.beginRenderPass(scenePass);

#ifndef SAIDA_RHI_WEBGPU
    if (gpuFrameActive_) {
        rp.setPipeline(*gpuDrivenPipeline_);
        rp.setBindGroup(0, *globalGroups_[currentFrame_]);
        rp.setBindGroup(1, resources_.globalMaterialSet());
        rp.setBindGroup(2, *cullingGroups_[currentFrame_]);

        PushConstants gfxPc{};
        gfxPc.model = glm::mat4(1.0f); // Unused in bindless
        gfxPc.params = glm::vec4(0.0f);
        rp.setPushConstants(&gfxPc, sizeof(PushConstants));

        auto& geometry = resources_.geometry();
        rp.setVertexBuffer(*geometry.vertexBuffer());
        rp.setIndexBuffer(*geometry.indexBuffer());

        if (currentInstanceCount_ > 0) {
            if (device_.capabilities().drawIndirectCount) {
                rp.drawIndexedIndirectCount(*drawCommandBuffers_[currentFrame_], 0,
                                            *countBuffers_[currentFrame_], 0,
                                            currentInstanceCount_,
                                            sizeof(gpu_driven::DrawIndexedIndirectCommand));
            } else {
                // Culled commands use instanceCount = 0 when indirect-count is unavailable.
                rp.drawIndexedIndirect(*drawCommandBuffers_[currentFrame_], 0,
                                       currentInstanceCount_,
                                       sizeof(gpu_driven::DrawIndexedIndirectCommand));
            }
        }
    } else
#endif
    {
        SAIDA_GPU_PROFILE_SCOPE(gpuProfiler, cmd, "Scene/Opaque");
        recordMeshDraws(rp, pipeline_.get(), false);
    }

    const auto& featureDrawList = featureDraws();
    FrameContext fc{encoder, rp,
                    currentFrame_, globalGroups_[currentFrame_].get(), scene,
                    Time::elapsed(), false, &camera, nullptr, false, extent,
                    featureDrawList.data(), static_cast<uint32_t>(featureDrawList.size())};
    {
        SAIDA_GPU_PROFILE_SCOPE(gpuProfiler, cmd, "Scene/Features");
        recordFeatures(fc);
    }
    recordWorldWebCanvases(rp, scene, camera);

    rp.end();
    }

    recordTonemapPass(encoder, imageIndex, scene, camera);

    encoder.transition(swapchain_->image(imageIndex), rhi::ResourceState::ColorAttachment,
                       rhi::ResourceState::Present);

    if (gpuProfiler) gpuProfiler->endZone(cmd, gpuFrameZone);
}

void Renderer::drawFrame(Scene& scene, Camera& camera, Project* project) {
    swapchain_->waitFrame(currentFrame_);
    if (Profiler::instance().enabled() && gpuProfiler_) {
        gpuProfiler_->beginFrame(currentFrame_);
        gpuProfiler_->publishLatest();
    }

    if (project) {
        SAIDA_PROFILE_SCOPE("Renderer/ProjectSettings");
#ifndef SAIDA_RHI_WEBGPU
        if (swapchain_->setVSync(project->vSync())) {
            cleanupHdrResources();
            createHdrResources();
            return;
        }
#endif

        if (shadowMap_->resize(project->shadowResolution())) {
            updateGlobalShadowDescriptor();
        }
    }

    uint32_t imageIndex;
    if (!swapchain_->acquire(currentFrame_, imageIndex)) {
        cleanupHdrResources();
        createHdrResources();
        return;
    }

    auto& settings = scene.settings();

    const int lightingMode = static_cast<int>(settings.lightingMode);
    const int giMode = static_cast<int>(settings.giMode);
    const bool giModeChanged = giMode != giLastMode_;
    const bool lightingModeChanged = lightingMode != giLastLightingMode_;
    const bool giEnabledChanged = settings.giEnabled != giWasEnabled_;
    const bool giHierarchyChanged = Node::g_hierarchyVersion != giLastHierarchyVersion_;
    const bool giTransformChanged = Node::g_transformVersion != giLastTransformVersion_;
    const uint64_t giSignature = giDirtySignature(scene);
    const bool giContentChanged = giSignature != giLastDirtySignature_;
    const bool giDirty = giHierarchyChanged || giTransformChanged || giContentChanged;
    if (giModeChanged || lightingModeChanged || giEnabledChanged || giHierarchyChanged) {
        giRealtimeWarmupRemaining_ = kGIRealtimeWarmupFrames;
        giLastMode_ = giMode;
        giLastLightingMode_ = lightingMode;
        giWasEnabled_ = settings.giEnabled;
    }

    // Decide whether the DDGI volume updates this frame. Full realtime updates
    // every frame; amortized realtime warms up then updates on a cadence.
    // Baked: a bake request runs the update for kGIBakeFrames frames to converge
    // the volume, then freezes it (direct lighting + shadows stay live in both
    // modes; only the indirect volume is frozen).
    if (settings.lightingMode == LightingMode::Realtime) {
        settings.bakeRequested = false;  // bake only applies in baked mode
        giUpdateThisFrame_ = settings.giEnabled && shouldUpdateRealtimeGI(giDirty);
        if (giUpdateThisFrame_ && giRealtimeWarmupRemaining_ > 0) {
            --giRealtimeWarmupRemaining_;
        }
    } else {  // Baked
        if (settings.bakeRequested) {
            settings.bakeRequested = false;
            giBakeFramesRemaining_ = kGIBakeFrames;
            settings.baked = false;
        }
        giUpdateThisFrame_ = giBakeFramesRemaining_ > 0;
        if (giUpdateThisFrame_ && --giBakeFramesRemaining_ == 0) {
            settings.baked = true;
        }
    }
    if (giUpdateThisFrame_ || !settings.giEnabled) {
        giLastHierarchyVersion_ = Node::g_hierarchyVersion;
        giLastTransformVersion_ = Node::g_transformVersion;
        giLastDirtySignature_ = giSignature;
    }
    ++giFrameCounter_;

    // Frozen GI retains its sampled atlas; updating GI swaps the ping-pong pair.
    if (giUpdateThisFrame_) gi_->beginFrame();
    {
        SAIDA_PROFILE_SCOPE("Renderer/UpdateDescriptors");
        updateGIDescriptors();
        updateEnvironmentDescriptor(scene);
    }

    rhi::Rect2D renderRect = activeRenderRect();
    {
        SAIDA_PROFILE_SCOPE("Renderer/UpdateUniforms");
        updateUniformBuffer(currentFrame_, scene, camera, project);
    }
    {
        SAIDA_PROFILE_SCOPE("UI/Gather");
        uiRenderer_->gatherUI(scene,
            {static_cast<float>(renderRect.extent.width), static_cast<float>(renderRect.extent.height)});
    }

    rhi::CommandEncoder encoder = swapchain_->beginFrameCommands(currentFrame_);
    {
        SAIDA_PROFILE_SCOPE("Renderer/RecordCommandBuffer");
        recordCommandBuffer(encoder, imageIndex, scene, camera);
    }
    if (swapchain_->submitAndPresent(encoder, currentFrame_, imageIndex)) {
        cleanupHdrResources();
        createHdrResources();
    }

    currentFrame_ = (currentFrame_ + 1) % kMaxFramesInFlight;
}

} // namespace saida

#ifdef SAIDA_ENABLE_XR
namespace saida {

namespace {
uint32_t xrViewMask(uint32_t viewCount) { return (1u << viewCount) - 1u; }
}

void Renderer::createXrTargets() {
    rhi::RenderTextureDesc hdrDesc;
    hdrDesc.format = rhi::Format::RGBA16Float;
    hdrDesc.width = xrExtent_.width;
    hdrDesc.height = xrExtent_.height;
    hdrDesc.layers = xrViewCount_;            // one layer per eye
    hdrDesc.usage = rhi::TextureUsage::ColorAttachment | rhi::TextureUsage::Sampled;
    hdrDesc.memoryCategory = "RenderTarget/XR";
    xrHdrTexture_ = std::make_unique<rhi::RenderTexture>(device_, hdrDesc);

    rhi::RenderTextureDesc depthDesc;
    depthDesc.format = rhi::vulkan::fromVk(device_.findDepthFormat());
    depthDesc.width = xrExtent_.width;
    depthDesc.height = xrExtent_.height;
    depthDesc.layers = xrViewCount_;
    depthDesc.usage = rhi::TextureUsage::DepthAttachment | rhi::TextureUsage::Sampled;
    depthDesc.memoryCategory = "RenderTarget/XR";
    xrDepthTexture_ = std::make_unique<rhi::RenderTexture>(device_, depthDesc);

    for (uint32_t i = 0; i < std::min<uint32_t>(xrViewCount_, 2); ++i) {
        xrPostProcessors_[i] = std::make_unique<PostProcessor>(device_, xrExtent_,
            rhi::Format::RGBA16Float, xrHdrTexture_->layerView(i));
    }
}

void Renderer::cleanupXrTargets() {
    for (auto& post : xrPostProcessors_) post.reset();
    xrDepthTexture_.reset();
    xrHdrTexture_.reset();
}

void Renderer::createXrPipelines() {
    const VkFormat hdrFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    const VkFormat depthFormat = device_.findDepthFormat();
    const uint32_t viewMask = xrViewMask(xrViewCount_);

    rhi::Pipeline::Desc sceneDesc;
    sceneDesc.colorFormats = {rhi::vulkan::fromVk(hdrFormat)};
    sceneDesc.depthFormat = rhi::vulkan::fromVk(depthFormat);
    sceneDesc.bindGroupLayouts = {globalSetLayout_.get(), &resources_.materialSetLayout()};
    sceneDesc.samples = 1;
    sceneDesc.viewMask = viewMask;

    sceneDesc.vertPath = shaderPath("multiview.shader.vert.spv");
    sceneDesc.fragPath = shaderPath("shader.frag.spv");
    xrScenePipeline_ = std::make_unique<rhi::Pipeline>(device_, sceneDesc);

    sceneDesc.fragPath = shaderPath("unlit.frag.spv");
    xrUnlitPipeline_ = std::make_unique<rhi::Pipeline>(device_, sceneDesc);

    if (resources_.globalMaterialSetLayout()) {
        rhi::Pipeline::Desc webDesc;
        webDesc.vertPath = shaderPath("multiview.web_canvas_world.vert.spv");
        webDesc.fragPath = shaderPath("web_canvas_world.frag.spv");
        webDesc.colorFormats = {rhi::vulkan::fromVk(hdrFormat)};
        webDesc.depthFormat = rhi::vulkan::fromVk(depthFormat);
        webDesc.bindGroupLayouts = {resources_.globalMaterialSetLayout()};  // raw bindless set
        webDesc.vertexInput = false;
        webDesc.depthWrite = false;
        webDesc.depthCompare = rhi::CompareOp::LessOrEqual;
        webDesc.cullMode = rhi::CullMode::None;
        webDesc.blendMode = rhi::BlendMode::Alpha;
        webDesc.topology = rhi::Topology::TriangleStrip;
        webDesc.pushConstantSize = sizeof(WebCanvasWorldPushConstants);
        webDesc.viewMask = viewMask;
        xrWebCanvasWorldPipeline_ = std::make_unique<rhi::Pipeline>(device_, webDesc);
    }

    buildFeatures(viewMask, rhi::vulkan::fromVk(depthFormat), VK_SAMPLE_COUNT_1_BIT);

    {
        tonemapSetLayout_ = std::make_unique<rhi::BindGroupLayout>(device_,
            std::vector<rhi::BindGroupLayoutEntry>{
                {0, rhi::BindingType::CombinedImageSampler, rhi::ShaderStages::Fragment},  // HDR
                {1, rhi::BindingType::CombinedImageSampler, rhi::ShaderStages::Fragment},  // depth (AO)
                {2, rhi::BindingType::CombinedImageSampler, rhi::ShaderStages::Fragment},  // bloom
            });

        rhi::SamplerDesc linearDesc;
        linearDesc.mipFilter = rhi::FilterMode::Linear;
        tonemapSampler_ = std::make_unique<rhi::Sampler>(device_, linearDesc);

        rhi::SamplerDesc nearestDesc;
        nearestDesc.magFilter = rhi::FilterMode::Nearest;
        nearestDesc.minFilter = rhi::FilterMode::Nearest;
        tonemapDepthSampler_ = std::make_unique<rhi::Sampler>(device_, nearestDesc);

        updateXrTonemapDescriptorSets();

        rhi::Pipeline::Desc xrTonemapDesc;
        xrTonemapDesc.vertPath = shaderPath("tonemap.vert.spv");
        xrTonemapDesc.fragPath = shaderPath("tonemap.frag.spv");
        xrTonemapDesc.colorFormats = {rhi::vulkan::fromVk(xrColorFormat_)};
        xrTonemapDesc.bindGroupLayouts = {tonemapSetLayout_.get()};
        xrTonemapDesc.vertexInput = false;
        xrTonemapDesc.depthTest = false;
        xrTonemapDesc.pushConstantSize = sizeof(TonemapPushConstants);
        xrTonemapPipeline_ = std::make_unique<rhi::Pipeline>(device_, xrTonemapDesc);
    }
}

void Renderer::updateXrTonemapDescriptorSets() {
    if (!tonemapSetLayout_ || !tonemapSampler_ || !tonemapDepthSampler_) return;
    const uint32_t n = std::min<uint32_t>(xrViewCount_, 2);
    for (uint32_t i = 0; i < n; ++i) {
        if (!xrPostProcessors_[i]) continue;

        rhi::BindGroupEntry hdrEntry;
        hdrEntry.binding = 0;
        hdrEntry.view = xrHdrTexture_->layerView(i);
        hdrEntry.sampler = tonemapSampler_->handle();

        rhi::BindGroupEntry depthEntry;
        depthEntry.binding = 1;
        depthEntry.view = xrDepthTexture_->layerView(i);
        depthEntry.sampler = tonemapDepthSampler_->handle();

        rhi::BindGroupEntry bloomEntry;
        bloomEntry.binding = 2;
        bloomEntry.view = xrPostProcessors_[i]->bloomView();
        bloomEntry.sampler = xrPostProcessors_[i]->bloomSampler();

        xrTonemapSets_[i] = std::make_unique<rhi::BindGroup>(*tonemapSetLayout_,
            std::vector<rhi::BindGroupEntry>{hdrEntry, depthEntry, bloomEntry});
    }
}

void Renderer::updateUniformBufferXr(uint32_t frame, const std::vector<EyeRenderInfo>& eyes,
                                     Scene& scene, Project* project) {
    UniformBufferObject ubo{};
    const uint32_t n = std::min<uint32_t>(static_cast<uint32_t>(eyes.size()), 2);
    for (uint32_t i = 0; i < n; ++i) {
        ubo.view[i] = eyes[i].view;
        ubo.proj[i] = eyes[i].projection;
    }
    if (n == 1) { ubo.view[1] = ubo.view[0]; ubo.proj[1] = ubo.proj[0]; }
    uniformBuffers_[frame]->write(&ubo, sizeof(ubo));

    // Specular/view-dependent terms use the eye centroid (head). No frustum cull:
    // a single combined-stereo frustum isn't worth the complexity here.
    glm::vec3 head(0.0f);
    for (uint32_t i = 0; i < n; ++i) head += eyes[i].eyePosition;
    head /= static_cast<float>(std::max(1u, n));

    LightingUBO lighting{};
    const glm::mat4* view = n > 0 ? &eyes[0].view : nullptr;
    const glm::mat4* proj = n > 0 ? &eyes[0].projection : nullptr;
    gatherScene(lighting, scene, head, nullptr, project, view, proj);
    lightingBuffers_[frame]->write(&lighting, sizeof(lighting));
}

void Renderer::recordXrScenePass(rhi::CommandEncoder& encoder, Scene& scene,
                                 const std::vector<EyeRenderInfo>& eyes) {
    auto& settings = scene.settings();
    // Passthrough (AR): clear fully transparent so the compositor blends the real
    // world through the background; opaque geometry (alpha 1) stays visible.
    const bool passthrough = XRPassthrough::enabled();
    glm::vec4 clearColor = settings.clearColor;
    if (passthrough) clearColor.a = 0.0f;

    // Feature pre-pass compute (GPU particle sim) — outside any render pass.
    {
        PrePassContext ppc{encoder, currentFrame_, scene, Time::elapsed(), true,
                           nullptr, &eyes, xrExtent_};
        for (auto& f : features_) f->recordPrePass(ppc);
    }

    // Both eye layers come back from sampling with stale contents (cleared below).
    encoder.transition(xrHdrTexture_->image(), rhi::ResourceState::ColorAttachment,
                       rhi::ResourceState::ColorAttachment, 0, xrViewCount_,
                       /*discardContents=*/true);
    encoder.transition(xrDepthTexture_->image(), rhi::ResourceState::DepthWrite,
                       rhi::ResourceState::DepthWrite, 0, xrViewCount_,
                       /*discardContents=*/true);

    rhi::RenderPassDesc pass;
    pass.colorCount = 1;
    pass.colors[0].view = xrHdrTexture_->view();
    pass.colors[0].loadOp = rhi::LoadOp::Clear;
    pass.colors[0].clearColor = {{clearColor.r, clearColor.g, clearColor.b, clearColor.a}};
    pass.depth.view = xrDepthTexture_->view();
    pass.depth.loadOp = rhi::LoadOp::Clear;
    pass.width = xrExtent_.width;
    pass.height = xrExtent_.height;
    pass.layerCount = 1;                        // ignored when viewMask != 0
    pass.viewMask = xrViewMask(xrViewCount_);   // multiview: both eyes in one pass

    rhi::RenderPassEncoder rp = encoder.beginRenderPass(pass);

    recordMeshDraws(rp, xrScenePipeline_.get(), true);

    // passthrough is forwarded so the skybox skips itself.
    const auto& featureDrawList = featureDraws();
    FrameContext fc{encoder, rp,
                    currentFrame_, globalGroups_[currentFrame_].get(), scene,
                    Time::elapsed(), true, nullptr, &eyes, passthrough, xrExtent_,
                    featureDrawList.data(), static_cast<uint32_t>(featureDrawList.size())};
    recordFeatures(fc);
    recordXrWorldWebCanvases(rp, scene, eyes);

    rp.end();
}

void Renderer::recordXrWorldWebCanvases(rhi::RenderPassEncoder& rp, Scene& scene,
                                        const std::vector<EyeRenderInfo>& eyes) {
    if (!xrWebCanvasWorldPipeline_ || !resources_.globalMaterialSet()) return;

    glm::vec3 head(0.0f);
    for (const EyeRenderInfo& eye : eyes) head += eye.eyePosition;
    head /= static_cast<float>(std::max<size_t>(1, eyes.size()));

    std::vector<WebCanvasWorldDraw> draws = collectWorldWebCanvasDraws(scene, head);
    if (draws.empty()) return;

    rp.setPipeline(*xrWebCanvasWorldPipeline_);
    rp.setBindGroup(0, resources_.globalMaterialSet());

    for (const WebCanvasWorldDraw& draw : draws) {
        WebCanvasNode* node = draw.node;
        Texture* texture = node->texture();
        if (!texture) continue;

        WebCanvasWorldPushConstants pc{};
        pc.model = node->worldTransform();
        pc.params = glm::vec4(node->worldWidth(), node->worldHeight(),
                              static_cast<float>(resources_.ensureBindlessTextureIndex(texture)), 1.0f);
        rp.setPushConstants(&pc, sizeof(pc));
        rp.draw(4);
    }
}

void Renderer::recordXrTonemap(rhi::CommandEncoder& encoder, Scene& scene,
                               const std::vector<EyeRenderInfo>& eyes) {
    VkCommandBuffer cmd = encoder.handle();  // GPU profiler zones (desktop tooling)
    GpuProfiler* gpuProfiler = Profiler::instance().enabled() ? gpuProfiler_.get() : nullptr;
    // HDR + depth (all eye layers) -> shader read for sampling.
    encoder.transition(xrHdrTexture_->image(), rhi::ResourceState::ColorAttachment,
                       rhi::ResourceState::ShaderRead, 0, xrViewCount_);
    encoder.transition(xrDepthTexture_->image(), rhi::ResourceState::DepthWrite,
                       rhi::ResourceState::ShaderRead, 0, xrViewCount_);

    const uint32_t n = std::min<uint32_t>(static_cast<uint32_t>(eyes.size()), xrViewCount_);
    {
    SAIDA_GPU_PROFILE_SCOPE(gpuProfiler, cmd, "GPU/Tonemap+EditorUI");
    for (uint32_t i = 0; i < n; ++i) {
        if (xrPostProcessors_[i]) {
            xrPostProcessors_[i]->recordBloom(encoder, scene.settings(),
                glm::vec4(0.0f, 0.0f, 1.0f, 1.0f), gpuProfiler);
        }
    }

    for (uint32_t i = 0; i < n; ++i) {
        const EyeRenderInfo& eye = eyes[i];
        // The XR image starts UNDEFINED; the compositor wants it left in
        // COLOR_ATTACHMENT_OPTIMAL, which is exactly where rendering leaves it.
        encoder.transition(eye.image, rhi::ResourceState::ColorAttachment,
                           rhi::ResourceState::ColorAttachment, 0, VK_REMAINING_ARRAY_LAYERS,
                           /*discardContents=*/true);

        rhi::RenderPassDesc pass;
        pass.colorCount = 1;
        pass.colors[0].view = eye.imageView;
        pass.colors[0].loadOp = rhi::LoadOp::DontCare;  // fully overwritten by the tonemap
        pass.width = eye.extent.width;
        pass.height = eye.extent.height;

        rhi::RenderPassEncoder rp = encoder.beginRenderPass(pass);
        rp.setPipeline(*xrTonemapPipeline_);
        rp.setBindGroup(0, *xrTonemapSets_[i]);
        TonemapPushConstants push = tonemapPushConstants(scene.settings(), eye.projection);
        {
            SAIDA_GPU_PROFILE_SCOPE(gpuProfiler, cmd, "Post/Tonemap");
            rp.setPushConstants(&push, sizeof(TonemapPushConstants));
            rp.draw(3);
        }
        rp.end();
    }
    }
}

void Renderer::drawXr(VkCommandBuffer cmd, const std::vector<EyeRenderInfo>& eyes,
                      Scene& scene, Project* project) {
    if (eyes.empty()) return;
    auto& settings = scene.settings();

    const int lightingMode = static_cast<int>(settings.lightingMode);
    const int giMode = static_cast<int>(settings.giMode);
    const bool giModeChanged = giMode != giLastMode_;
    const bool lightingModeChanged = lightingMode != giLastLightingMode_;
    const bool giEnabledChanged = settings.giEnabled != giWasEnabled_;
    const bool giHierarchyChanged = Node::g_hierarchyVersion != giLastHierarchyVersion_;
    const bool giTransformChanged = Node::g_transformVersion != giLastTransformVersion_;
    const uint64_t giSignature = giDirtySignature(scene);
    const bool giContentChanged = giSignature != giLastDirtySignature_;
    const bool giDirty = giHierarchyChanged || giTransformChanged || giContentChanged;
    if (giModeChanged || lightingModeChanged || giEnabledChanged || giHierarchyChanged) {
        giRealtimeWarmupRemaining_ = kGIRealtimeWarmupFrames;
        giLastMode_ = giMode;
        giLastLightingMode_ = lightingMode;
        giWasEnabled_ = settings.giEnabled;
    }

    if (settings.lightingMode == LightingMode::Realtime) {
        giUpdateThisFrame_ = settings.giEnabled &&
            shouldUpdateRealtimeGI(giDirty);
        if (giUpdateThisFrame_ && giRealtimeWarmupRemaining_ > 0) {
            --giRealtimeWarmupRemaining_;
        }
    } else {
        if (settings.bakeRequested) {
            settings.bakeRequested = false;
            giBakeFramesRemaining_ = kGIBakeFrames;
            settings.baked = false;
        }
        giUpdateThisFrame_ = giBakeFramesRemaining_ > 0;
        if (giUpdateThisFrame_ && --giBakeFramesRemaining_ == 0)
            settings.baked = true;
    }
    if (giUpdateThisFrame_ || !settings.giEnabled) {
        giLastHierarchyVersion_ = Node::g_hierarchyVersion;
        giLastTransformVersion_ = Node::g_transformVersion;
        giLastDirtySignature_ = giSignature;
    }
    ++giFrameCounter_;

    if (giUpdateThisFrame_) gi_->beginFrame();
    updateGIDescriptors();
    updateEnvironmentDescriptor(scene);

    updateUniformBufferXr(currentFrame_, eyes, scene, project);

    rhi::CommandEncoder encoder(cmd);
    if (giUpdateThisFrame_) {
        GpuProfiler* gpuProfiler = Profiler::instance().enabled() ? gpuProfiler_.get() : nullptr;
        SAIDA_GPU_PROFILE_SCOPE(gpuProfiler, cmd, "GPU/DDGI");
        gi_->voxelize(encoder, scene, gpuProfiler);
        gi_->update(encoder, *globalGroups_[currentFrame_], gpuProfiler);
    }
    recordShadowPasses(encoder);

    recordXrScenePass(encoder, scene, eyes);
    recordXrTonemap(encoder, scene, eyes);

    currentFrame_ = (currentFrame_ + 1) % kMaxFramesInFlight;
}

} // namespace saida
#endif // SAIDA_ENABLE_XR
