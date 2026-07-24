#pragma once

#include "rhi/Rhi.hpp"

#ifdef SAIDA_RHI_WEBGPU
#include "graphics/Buffer.hpp"
#include "graphics/ComputePipeline.hpp"
#endif

#include <glm/glm.hpp>

#include <array>
#include <functional>
#include <memory>
#include <random>

namespace saida {

#ifndef SAIDA_RHI_WEBGPU
class Buffer;
class ComputePipeline;
#endif
class Scene;
class GpuProfiler;

// Layout of the DDGI probe grid. Visibility stores mean distance and mean^2.
struct GIVolumeDesc {
    glm::vec3 origin{-12.0f, -4.0f, -12.0f};  // world-space min corner
    glm::vec3 spacing{3.0f, 3.0f, 3.0f};      // probe spacing (world units)
    glm::ivec3 counts{8, 4, 8};               // probes per axis
    int irradianceTexels = 8;                 // octahedral resolution / probe
    int visibilityTexels = 16;
    int voxelResolution = 64;                 // cubic voxel grid resolution
    int raysPerProbe = 64;                    // DDGI rays cast per probe per update
    float hysteresis = 0.97f;                 // temporal blend (Majercik 2019)
    float distExponent = 50.0f;               // visibility distance weight sharpness

    int probeCount() const { return counts.x * counts.y * counts.z; }
};

// Owns ping-pong irradiance/visibility atlases plus the sampler used by lighting.
// Baked mode freezes the current atlases.
class GIVolume {
public:
    GIVolume(rhi::Device& device, const GIVolumeDesc& desc,
             rhi::BindGroupLayout& materialSetLayout, rhi::BindGroupLayout& globalSetLayout);
    ~GIVolume();
    GIVolume(const GIVolume&) = delete;
    GIVolume& operator=(const GIVolume&) = delete;

    // Re-voxelize the scene's albedo into the 3D grid (call before the main pass).
    // Clears, runs the 3 dominant-axis passes, and leaves the grid sampled-ready.
    void voxelize(rhi::CommandEncoder& encoder, Scene& scene, GpuProfiler* profiler = nullptr);
    using DrawGeometryFn = std::function<void(rhi::RenderPassEncoder&, uint32_t axis)>;
    void voxelize(rhi::CommandEncoder& encoder, const DrawGeometryFn& drawGeometry,
                  GpuProfiler* profiler = nullptr);

    // Swap the ping-pong atlases (call host-side at frame start). After this, the
    // "current" atlas is the one update() writes and the lighting pass samples;
    // the other holds the previous frame's result (read for temporal blending).
    void beginFrame() { curr_ ^= 1; }

    // Run the DDGI update: trace rays, blend into the current atlases, copy borders.
    // globalSet = set 0 for this frame (lights, shadow maps, voxel grid).
    void update(rhi::CommandEncoder& encoder, const rhi::BindGroup& globalSet,
                GpuProfiler* profiler = nullptr);

    // Views/sampler the renderer binds into set 0 (bindings 4/5/6) for shading.
    rhi::TextureView irradianceView() const { return irradiance_[curr_]->view(); }
    rhi::TextureView visibilityView() const { return visibility_[curr_]->view(); }
    rhi::TextureView voxelView() const { return voxelTexture_->view(); }
    rhi::SamplerHandle sampler() const { return sampler_->handle(); }

    const GIVolumeDesc& desc() const { return desc_; }
    int probesPerRow() const { return probesPerRow_; }
    // World-space size of the volume box (probe lattice extent).
    glm::vec3 worldExtent() const { return glm::vec3(desc_.counts - 1) * desc_.spacing; }

    // Atlas pixel dimensions (probesPerRow tiles wide, with a 1px border/probe).
    glm::ivec2 irradianceAtlasSize() const { return atlasSize(desc_.irradianceTexels); }
    glm::ivec2 visibilityAtlasSize() const { return atlasSize(desc_.visibilityTexels); }

private:
    void createSampler();
    void fillConstant();  // P0: clear atlases to a constant via a one-time command
    void createVoxelResources(rhi::BindGroupLayout& materialSetLayout);
    void createComputeResources(rhi::BindGroupLayout& globalSetLayout);

    glm::ivec2 atlasSize(int texels) const {
        int rows = (desc_.probeCount() + probesPerRow_ - 1) / probesPerRow_;
        return {probesPerRow_ * (texels + 2), rows * (texels + 2)};
    }

    rhi::Device& device_;
    GIVolumeDesc desc_;
    int probesPerRow_ = 1;
    int curr_ = 0;

    std::array<std::unique_ptr<rhi::RenderTexture>, 2> irradiance_;
    std::array<std::unique_ptr<rhi::RenderTexture>, 2> visibility_;
    std::unique_ptr<rhi::Sampler> sampler_;

    std::unique_ptr<rhi::RenderTexture> voxelTexture_;
    std::unique_ptr<Buffer> voxelUbo_;
    std::unique_ptr<rhi::BindGroupLayout> voxelSetLayout_;
    std::unique_ptr<rhi::BindGroup> voxelSet_;
    std::unique_ptr<rhi::Pipeline> voxelPipeline_;
#ifdef SAIDA_RHI_WEBGPU
    // WebGPU requires an attachment even though voxelization writes only an image.
    std::unique_ptr<rhi::RenderTexture> voxelDummyTarget_;
#endif

    std::unique_ptr<Buffer> raysBuffer_;
    std::unique_ptr<rhi::BindGroupLayout> giComputeSetLayout_;
    std::array<std::unique_ptr<rhi::BindGroup>, 2> giComputeSets_;
    std::unique_ptr<ComputePipeline> tracePipeline_;
    std::unique_ptr<ComputePipeline> blendPipeline_;
    std::unique_ptr<ComputePipeline> borderPipeline_;
    std::mt19937 rng_{1234};
};

} // namespace saida
