#include "render/GIVolume.hpp"

#ifndef SAIDA_RHI_WEBGPU
#include "graphics/VulkanDevice.hpp"
#include "rhi/vulkan/Format.hpp"
#endif
#include "graphics/Buffer.hpp"
#include "graphics/Mesh.hpp"
#include "graphics/Material.hpp"
#include "graphics/GpuProfiler.hpp"
#include "graphics/ComputePipeline.hpp"
#include "scene/Scene.hpp"
#include "nodes/MeshNode.hpp"
#include "core/Paths.hpp"
#include "core/Log.hpp"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <array>
#include <cmath>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace saida {

namespace {
constexpr rhi::Format kIrradianceFormat = rhi::Format::RGBA16Float;
#ifdef SAIDA_RHI_WEBGPU
// rg16f is not a storage format in WebGPU core: the visibility atlas uses
// rgba16f instead (ba channels unused) to stay writable in compute AND
// filterable when sampled. Mirrors the -DWEB variant of ddgi_blend.comp.
constexpr rhi::Format kVisibilityFormat = rhi::Format::RGBA16Float;
#else
constexpr rhi::Format kVisibilityFormat = rhi::Format::RG16Float;
#endif
constexpr rhi::Format kVoxelFormat = rhi::Format::RGBA16Float;
// Atlas data is compute-written, sampled, and initialized through a copy.
constexpr rhi::TextureUsage kAtlasUsage =
    rhi::TextureUsage::Storage | rhi::TextureUsage::Sampled | rhi::TextureUsage::CopyDst;

struct VoxelUBOData {
    glm::vec4 origin;
    glm::vec4 extent;
    glm::ivec4 res;
    glm::mat4 axisVP[3];
};

struct VoxelPush {
    glm::mat4 model;
    uint32_t axis;
};

struct RayData {
    glm::vec4 dirDist;
    glm::vec4 radiance;
};

struct TracePush {
    glm::mat4 randomRot;
    int32_t raysPerProbe;
    int32_t probeCount;
};
struct BlendPush {
    int32_t mode;
    int32_t raysPerProbe;
    int32_t probeCount;
    float hysteresis;
    float distExponent;
};
struct BorderPush {
    int32_t mode;
    int32_t probeCount;
};
}

GIVolume::GIVolume(rhi::Device& device, const GIVolumeDesc& desc,
                   rhi::BindGroupLayout& materialSetLayout, rhi::BindGroupLayout& globalSetLayout)
    : device_(device), desc_(desc) {
    probesPerRow_ = std::max(1, static_cast<int>(std::ceil(std::sqrt(
                        static_cast<double>(desc_.probeCount())))));

    glm::ivec2 irr = irradianceAtlasSize();
    glm::ivec2 vis = visibilityAtlasSize();
    auto makeAtlas = [&](rhi::Format format, glm::ivec2 size) {
        rhi::RenderTextureDesc desc;
        desc.format = format;
        desc.width = static_cast<uint32_t>(size.x);
        desc.height = static_cast<uint32_t>(size.y);
        desc.usage = kAtlasUsage;
        return std::make_unique<rhi::RenderTexture>(device_, desc);
    };
    for (int i = 0; i < 2; ++i) {
        irradiance_[i] = makeAtlas(kIrradianceFormat, irr);
        visibility_[i] = makeAtlas(kVisibilityFormat, vis);
    }

    createSampler();
    fillConstant();
    createVoxelResources(materialSetLayout);
    createComputeResources(globalSetLayout);

    Log::info("GIVolume: ", desc_.probeCount(), " probes (", desc_.counts.x, "x",
              desc_.counts.y, "x", desc_.counts.z, "), irradiance atlas ", irr.x,
              "x", irr.y, ", visibility atlas ", vis.x, "x", vis.y,
              ", voxel grid ", desc_.voxelResolution, "^3");
}

GIVolume::~GIVolume() = default;  // everything is RAII

void GIVolume::createSampler() {
    sampler_ = std::make_unique<rhi::Sampler>(device_, rhi::SamplerDesc{});
}

void GIVolume::fillConstant() {
    // Irradiance stores mean incident radiance (diffuse = albedo * value) — a
    // neutral ambient until the first DDGI update writes real data. Visibility:
    // huge mean distance so the Chebyshev test always returns "fully visible"
    // (no occlusion) until real data is baked/updated.
    const std::array<float, 4> irr{0.10f, 0.10f, 0.10f, 1.0f};
    const std::array<float, 4> vis{1.0e4f, 1.0e8f, 0.0f, 0.0f};  // mean dist, dist^2

    device_.withSingleTimeEncoder([&](rhi::CommandEncoder& enc) {
        auto clear = [&](rhi::RenderTexture& img, const std::array<float, 4>& color) {
            enc.transition(img.image(), rhi::ResourceState::Undefined,
                           rhi::ResourceState::CopyDst);
            enc.clearColorTexture(img.image(), color);
            // Atlases live in GENERAL: compute writes them as storage images and
            // the lighting pass samples them in GENERAL (no per-frame churn).
            enc.transition(img.image(), rhi::ResourceState::CopyDst,
                           rhi::ResourceState::StorageReadWrite);
        };
        for (int i = 0; i < 2; ++i) {
            clear(*irradiance_[i], irr);
            clear(*visibility_[i], vis);
        }
    });
}

void GIVolume::createVoxelResources(rhi::BindGroupLayout& materialSetLayout) {
    const uint32_t res = static_cast<uint32_t>(desc_.voxelResolution);

    rhi::RenderTextureDesc voxelDesc;
    voxelDesc.format = kVoxelFormat;
    voxelDesc.width = res;
    voxelDesc.height = res;
    voxelDesc.depth = res;
    voxelDesc.usage = kAtlasUsage;
    voxelTexture_ = std::make_unique<rhi::RenderTexture>(device_, voxelDesc);

    voxelUbo_ = std::make_unique<Buffer>(device_, sizeof(VoxelUBOData),
        rhi::BufferUsage::Uniform, MemoryUsage::HostVisible);

#ifdef SAIDA_RHI_WEBGPU
    rhi::webgpu::BindGroupLayoutEntry voxelImage{};
    voxelImage.binding = 0;
    voxelImage.type = rhi::BindingType::StorageImage;
    voxelImage.visibility = rhi::ShaderStages::Fragment;
    voxelImage.dim = rhi::webgpu::TextureDim::Dim3D;
    voxelImage.storageFormat = kVoxelFormat;
    voxelImage.storageAccess = WGPUStorageTextureAccess_WriteOnly;
    rhi::webgpu::BindGroupLayoutEntry voxelUbo{};
    voxelUbo.binding = 1;
    voxelUbo.type = rhi::BindingType::UniformBuffer;
    voxelUbo.visibility = rhi::ShaderStages::Vertex | rhi::ShaderStages::Fragment;
    voxelSetLayout_ = std::make_unique<rhi::BindGroupLayout>(device_,
        std::vector<rhi::webgpu::BindGroupLayoutEntry>{voxelImage, voxelUbo});
#else
    voxelSetLayout_ = std::make_unique<rhi::BindGroupLayout>(device_,
        std::vector<rhi::BindGroupLayoutEntry>{
            {0, rhi::BindingType::StorageImage, rhi::ShaderStages::Fragment},
            {1, rhi::BindingType::UniformBuffer, rhi::ShaderStages::Vertex | rhi::ShaderStages::Fragment},
        });
#endif

    rhi::BindGroupEntry voxelImageEntry;
    voxelImageEntry.binding = 0;
    voxelImageEntry.view = voxelTexture_->view();
#ifndef SAIDA_RHI_WEBGPU
    voxelImageEntry.textureState = rhi::ResourceState::StorageReadWrite;
#endif
    rhi::BindGroupEntry uboEntry;
    uboEntry.binding = 1;
    uboEntry.buffer = voxelUbo_.get();
    uboEntry.range = sizeof(VoxelUBOData);
    voxelSet_ = std::make_unique<rhi::BindGroup>(*voxelSetLayout_,
        std::vector<rhi::BindGroupEntry>{voxelImageEntry, uboEntry});

    rhi::Pipeline::Desc pipelineDesc;
#ifdef SAIDA_RHI_WEBGPU
    pipelineDesc.vertPath = "/shaders/voxelize.vert.wgsl";
    pipelineDesc.fragPath = "/shaders/voxelize.frag.wgsl";
    // WebGPU rejects attachment-less passes ("Render pass has no attachments"),
    // which silently invalidates the whole frame submit. Rasterize into a
    // throwaway res×res target with writeMask None instead.
    pipelineDesc.colorFormats = {rhi::Format::RGBA8Unorm};
    pipelineDesc.colorWrite = false;
    {
        rhi::RenderTextureDesc dummyDesc;
        dummyDesc.format = rhi::Format::RGBA8Unorm;
        dummyDesc.width = res;
        dummyDesc.height = res;
        dummyDesc.usage = rhi::TextureUsage::ColorAttachment;
        voxelDummyTarget_ = std::make_unique<rhi::RenderTexture>(device_, dummyDesc);
    }
#else
    pipelineDesc.vertPath = shaderPath("voxelize.vert.spv");
    pipelineDesc.fragPath = shaderPath("voxelize.frag.spv");
#endif
    pipelineDesc.bindGroupLayouts = {voxelSetLayout_.get(), &materialSetLayout};
    pipelineDesc.depthTest = false;
    pipelineDesc.depthWrite = false;
    pipelineDesc.cullMode = rhi::CullMode::None;
    pipelineDesc.pushConstantSize = sizeof(VoxelPush);
    pipelineDesc.pushConstantStages = rhi::ShaderStages::Vertex;
    voxelPipeline_ = std::make_unique<rhi::Pipeline>(device_, pipelineDesc);
}

void GIVolume::voxelize(rhi::CommandEncoder& encoder, const DrawGeometryFn& drawGeometry,
                        GpuProfiler* profiler) {
    SAIDA_GPU_PROFILE_SCOPE(profiler, encoder.handle(), "DDGI/Voxelize");
    const uint32_t res = static_cast<uint32_t>(desc_.voxelResolution);
    glm::vec3 extent = worldExtent();
    glm::vec3 center = desc_.origin + extent * 0.5f;

    VoxelUBOData ubo{};
    ubo.origin = glm::vec4(desc_.origin, 0.0f);
    ubo.extent = glm::vec4(extent, 0.0f);
    ubo.res = glm::ivec4(res, res, res, 0);
    auto axisVP = [&](glm::vec3 dir, glm::vec3 up, float w, float h, float depth) {
        glm::mat4 v = glm::lookAt(center + dir * depth, center, up);
        glm::mat4 p = glm::ortho(-w * 0.5f, w * 0.5f, -h * 0.5f, h * 0.5f, 0.0f, 2.0f * depth);
        return p * v;
    };
    ubo.axisVP[0] = axisVP({0, 0, 1}, {0, 1, 0}, extent.x, extent.y, extent.z); // along Z
    ubo.axisVP[1] = axisVP({1, 0, 0}, {0, 1, 0}, extent.z, extent.y, extent.x); // along X
    ubo.axisVP[2] = axisVP({0, 1, 0}, {0, 0, 1}, extent.x, extent.z, extent.y); // along Y
    voxelUbo_->write(&ubo, sizeof(ubo));

    encoder.transition(voxelTexture_->image(), rhi::ResourceState::Undefined,
                       rhi::ResourceState::CopyDst);
    encoder.clearColorTexture(voxelTexture_->image(), {0.0f, 0.0f, 0.0f, 0.0f});
    encoder.transition(voxelTexture_->image(), rhi::ResourceState::CopyDst,
                       rhi::ResourceState::StorageReadWrite);

    rhi::RenderPassDesc passDesc;
    passDesc.width = res;
    passDesc.height = res;
#ifdef SAIDA_RHI_WEBGPU
    // Web: attachment-less passes are invalid — bind the throwaway target
    // (the pipeline masks all writes to it).
    passDesc.colorCount = 1;
    passDesc.colors[0].view = voxelDummyTarget_->view();
    passDesc.colors[0].store = false;
#endif
    rhi::RenderPassEncoder rp = encoder.beginRenderPass(passDesc);
    rp.setPipeline(*voxelPipeline_);
    rp.setBindGroup(0, *voxelSet_);

    for (uint32_t axis = 0; axis < 3; ++axis) {
        drawGeometry(rp, axis);
    }

    rp.end();

    encoder.transition(voxelTexture_->image(), rhi::ResourceState::StorageReadWrite,
                       rhi::ResourceState::ShaderRead);
}

void GIVolume::voxelize(rhi::CommandEncoder& encoder, Scene& scene, GpuProfiler* profiler) {
    voxelize(encoder, [&](rhi::RenderPassEncoder& rp, uint32_t axis) {
        for (MeshNode* node : scene.meshes()) {
            Mesh* mesh = node->mesh();
            Material* mat = node->material();
            if (!mesh || !mat) continue;
            rp.setBindGroup(1, mat->descriptorSet());
            VoxelPush pc{node->worldTransform(), axis};
            rp.setPushConstants(&pc, sizeof(pc));
            mesh->bind(rp);
            mesh->draw(rp);
        }
    }, profiler);
}

void GIVolume::createComputeResources(rhi::BindGroupLayout& globalSetLayout) {
    const int probeCount = desc_.probeCount();
    raysBuffer_ = std::make_unique<Buffer>(device_,
        static_cast<uint64_t>(probeCount) * desc_.raysPerProbe * sizeof(RayData),
        rhi::BufferUsage::Storage, MemoryUsage::GpuOnly);

#ifdef SAIDA_RHI_WEBGPU
    auto storage = [](uint32_t binding, rhi::Format format, WGPUStorageTextureAccess access) {
        rhi::webgpu::BindGroupLayoutEntry e{};
        e.binding = binding;
        e.type = rhi::BindingType::StorageImage;
        e.visibility = rhi::ShaderStages::Compute;
        e.storageFormat = format;
        e.storageAccess = access;
        return e;
    };
    rhi::webgpu::BindGroupLayoutEntry rays{};
    rays.binding = 0;
    rays.type = rhi::BindingType::StorageBuffer;
    rays.visibility = rhi::ShaderStages::Compute;
    // Write targets are WriteOnly: read-write storage is r32*-only in WebGPU
    // core (the border copy that needed it is folded into ddgi_blend on web).
    giComputeSetLayout_ = std::make_unique<rhi::BindGroupLayout>(device_,
        std::vector<rhi::webgpu::BindGroupLayoutEntry>{
            rays,
            storage(1, kIrradianceFormat, WGPUStorageTextureAccess_WriteOnly),
            storage(2, kVisibilityFormat, WGPUStorageTextureAccess_WriteOnly),
            storage(3, kIrradianceFormat, WGPUStorageTextureAccess_ReadOnly),
            storage(4, kVisibilityFormat, WGPUStorageTextureAccess_ReadOnly),
        });
#else
    giComputeSetLayout_ = std::make_unique<rhi::BindGroupLayout>(device_,
        std::vector<rhi::BindGroupLayoutEntry>{
            {0, rhi::BindingType::StorageBuffer, rhi::ShaderStages::Compute},
            {1, rhi::BindingType::StorageImage, rhi::ShaderStages::Compute},
            {2, rhi::BindingType::StorageImage, rhi::ShaderStages::Compute},
            {3, rhi::BindingType::StorageImage, rhi::ShaderStages::Compute},
            {4, rhi::BindingType::StorageImage, rhi::ShaderStages::Compute},
        });
#endif

    for (int p = 0; p < 2; ++p) {
        rhi::BindGroupEntry raysEntry;
        raysEntry.binding = 0;
        raysEntry.buffer = raysBuffer_.get();
        auto imgEntry = [](uint32_t binding, rhi::TextureView v) {
            rhi::BindGroupEntry e; e.binding = binding; e.view = v;
#ifndef SAIDA_RHI_WEBGPU
            e.textureState = rhi::ResourceState::StorageReadWrite;
#endif
            return e;
        };
        rhi::BindGroupEntry wi = imgEntry(1, irradiance_[p]->view());
        rhi::BindGroupEntry wv = imgEntry(2, visibility_[p]->view());
        rhi::BindGroupEntry pi = imgEntry(3, irradiance_[1 - p]->view());
        rhi::BindGroupEntry pv = imgEntry(4, visibility_[1 - p]->view());
        giComputeSets_[p] = std::make_unique<rhi::BindGroup>(*giComputeSetLayout_,
            std::vector<rhi::BindGroupEntry>{raysEntry, wi, wv, pi, pv});
    }

#ifdef SAIDA_RHI_WEBGPU
    std::vector<const rhi::BindGroupLayout*> setLayouts = {&globalSetLayout, giComputeSetLayout_.get()};
    tracePipeline_  = std::make_unique<ComputePipeline>(device_, "/shaders/ddgi_trace.comp.wgsl", setLayouts, sizeof(TracePush));
    blendPipeline_  = std::make_unique<ComputePipeline>(device_, "/shaders/ddgi_blend.comp.wgsl", setLayouts, sizeof(BlendPush));
#else
    std::vector<rhi::vulkan::BindGroupLayoutRef> setLayouts = {globalSetLayout, *giComputeSetLayout_};
    tracePipeline_  = std::make_unique<ComputePipeline>(device_, shaderPath("ddgi_trace.comp.spv"), setLayouts, sizeof(TracePush));
    blendPipeline_  = std::make_unique<ComputePipeline>(device_, shaderPath("ddgi_blend.comp.spv"), setLayouts, sizeof(BlendPush));
    borderPipeline_ = std::make_unique<ComputePipeline>(device_, shaderPath("ddgi_borders.comp.spv"), setLayouts, sizeof(BorderPush));
#endif
}

void GIVolume::update(rhi::CommandEncoder& encoder, const rhi::BindGroup& globalSet,
                      GpuProfiler* profiler) {
    const int probeCount = desc_.probeCount();
    const rhi::BindGroup& giSet = *giComputeSets_[curr_];

    encoder.storageBarrier();

    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
    glm::quat q = glm::normalize(glm::quat(dist(rng_), dist(rng_), dist(rng_), dist(rng_)));
    glm::mat4 randomRot = glm::mat4_cast(q);

    rhi::ComputePassEncoder cp = encoder.beginComputePass();

    {
        SAIDA_GPU_PROFILE_SCOPE(profiler, encoder.handle(), "DDGI/Trace");
        cp.setPipeline(*tracePipeline_);
        cp.setBindGroup(0, globalSet);
        cp.setBindGroup(1, giSet);
        TracePush tp{randomRot, desc_.raysPerProbe, probeCount};
        cp.setPushConstants(&tp, sizeof(tp));
        uint32_t rayGroups = (static_cast<uint32_t>(probeCount * desc_.raysPerProbe) + 63) / 64;
        cp.dispatch(rayGroups);
    }

    encoder.storageBarrier();  // rays write -> blend read

    {
        SAIDA_GPU_PROFILE_SCOPE(profiler, encoder.handle(), "DDGI/Blend");
        cp.setPipeline(*blendPipeline_);
        cp.setBindGroup(0, globalSet);
        cp.setBindGroup(1, giSet);
        auto blend = [&](int mode, glm::ivec2 atlas) {
            BlendPush bp{mode, desc_.raysPerProbe, probeCount, desc_.hysteresis, desc_.distExponent};
            cp.setPushConstants(&bp, sizeof(bp));
            cp.dispatch((atlas.x + 7) / 8, (atlas.y + 7) / 8);
        };
        blend(0, irradianceAtlasSize());
        blend(1, visibilityAtlasSize());
    }

#ifndef SAIDA_RHI_WEBGPU
    encoder.storageBarrier();  // blend write -> border read

    {
        SAIDA_GPU_PROFILE_SCOPE(profiler, encoder.handle(), "DDGI/Borders");
        cp.setPipeline(*borderPipeline_);
        cp.setBindGroup(0, globalSet);
        cp.setBindGroup(1, giSet);
        auto border = [&](int mode, glm::ivec2 atlas) {
            BorderPush bp{mode, probeCount};
            cp.setPushConstants(&bp, sizeof(bp));
            cp.dispatch((atlas.x + 7) / 8, (atlas.y + 7) / 8);
        };
        border(0, irradianceAtlasSize());
        border(1, visibilityAtlasSize());
    }
#endif
    cp.end();

    encoder.computeToGraphicsBarrier();
}

} // namespace saida
