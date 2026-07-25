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

    std::unique_ptr<rhi::Pipeline> pipeline_;  // depth-only, vertex-stage push constants
};

} // namespace saida
