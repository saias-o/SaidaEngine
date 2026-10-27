#pragma once

#include "rhi/Rhi.hpp"

#include <functional>
#include <memory>

namespace saida {

class ShadowMap {
public:
    static constexpr uint32_t kMaxShadows = 4;

    // `globalLayout` is the renderer's set 0. The depth pass needs it because a
    // skinned caster reads its bone palette from that set: without it the shadow
    // could only ever be cast from the bind pose.
    ShadowMap(rhi::Device& device, const rhi::BindGroupLayout* globalLayout,
              uint32_t initialResolution = 2048);
    ~ShadowMap();
    ShadowMap(const ShadowMap&) = delete;
    ShadowMap& operator=(const ShadowMap&) = delete;

    // Resizing invalidates arrayView(), so dependent descriptors must be rebuilt.
    bool resize(uint32_t newResolution);

    using DrawGeometryFn = std::function<void(rhi::RenderPassEncoder&, int layer)>;

    void record(rhi::CommandEncoder& encoder, int count, const DrawGeometryFn& drawGeometry);

    rhi::TextureView arrayView() const { return texture_->view(); }
    rhi::SamplerHandle sampler() const { return sampler_->handle(); }
#ifdef SAIDA_RHI_WEBGPU
    // Stands in for arrayView() in the set 0 the depth pass binds: WebGPU
    // rejects a texture bound for sampling in the pass that renders into it,
    // even though the depth-only shader never reads that binding.
    rhi::TextureView casterPlaceholderView() const { return placeholder_->view(); }
#endif
    uint32_t resolution() const { return resolution_; }
    float depthReferenceBias() const {
        // Include interpolation and model/light matrix reassociation, not only
        // stored-depth quantization: a large triangle can take a different
        // floating-point path in the scene and depth-only vertex stages.
        return format_ == rhi::Format::Depth16 ? 1.5f / 65535.0f : 2.0e-5f;
    }

private:
    void createTexture();
    void createSampler();
    void createPipeline();

    rhi::Device& device_;
    const rhi::BindGroupLayout* globalLayout_ = nullptr;
    rhi::Format format_ = rhi::Format::Undefined;
    uint32_t resolution_;

    std::unique_ptr<rhi::RenderTexture> texture_;  // kMaxShadows depth layers
    std::unique_ptr<rhi::Sampler> sampler_;
#ifdef SAIDA_RHI_WEBGPU
    std::unique_ptr<rhi::RenderTexture> placeholder_;  // 1x1, same format and layers
#endif

    std::unique_ptr<rhi::Pipeline> pipeline_;  // depth-only, vertex-stage push constants
};

} // namespace saida
