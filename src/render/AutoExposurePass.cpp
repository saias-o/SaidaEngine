#include "render/AutoExposurePass.hpp"

#include "core/Paths.hpp"
#ifndef SAIDA_RHI_WEBGPU
#include "graphics/VulkanDevice.hpp"
#endif
#include "scene/Scene.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace saida {

AutoExposurePass::AutoExposurePass(rhi::Device& device) {
#ifdef SAIDA_RHI_WEBGPU
    using WE = rhi::webgpu::BindGroupLayoutEntry;
    std::vector<WE> entries;
    WE e{};
    e.binding = 0; e.type = rhi::BindingType::SampledTexture; e.visibility = rhi::ShaderStages::Compute;
    entries.push_back(e);
    e = {}; e.binding = 1; e.type = rhi::BindingType::Sampler; e.visibility = rhi::ShaderStages::Compute;
    entries.push_back(e);
    e = {}; e.binding = 2; e.type = rhi::BindingType::StorageBuffer; e.visibility = rhi::ShaderStages::Compute;
    entries.push_back(e);
    layout_ = std::make_unique<rhi::BindGroupLayout>(device, entries);
    std::vector<const rhi::BindGroupLayout*> layouts = {layout_.get()};
    pipeline_ = std::make_unique<ComputePipeline>(device, "/shaders/auto_exposure.comp.wgsl", layouts,
                                                  sizeof(PushConstants));
#else
    layout_ = std::make_unique<rhi::BindGroupLayout>(device, std::vector<rhi::BindGroupLayoutEntry>{
        {0, rhi::BindingType::CombinedImageSampler, rhi::ShaderStages::Compute},
        {2, rhi::BindingType::StorageBuffer, rhi::ShaderStages::Compute},
    });
    std::vector<rhi::vulkan::BindGroupLayoutRef> layouts = {*layout_};
    pipeline_ = std::make_unique<ComputePipeline>(device, shaderPath("auto_exposure.comp.spv"), layouts,
                                                  sizeof(PushConstants));
#endif
    sampler_ = std::make_unique<rhi::Sampler>(device, rhi::SamplerDesc{});
    // Zero: not yet metered, so the first frame takes its target at once.
    state_ = std::make_unique<Buffer>(device, sizeof(glm::vec4), rhi::BufferUsage::Storage,
                                      MemoryUsage::HostVisible);
    const glm::vec4 zero(0.0f);
    state_->write(&zero, sizeof(zero));
}

AutoExposurePass::~AutoExposurePass() = default;

void AutoExposurePass::setInput(rhi::TextureView hdr) {
    rhi::BindGroupEntry image;
    image.binding = 0;
    image.view = hdr;
    rhi::BindGroupEntry buffer;
    buffer.binding = 2;
    buffer.buffer = state_.get();
#ifdef SAIDA_RHI_WEBGPU
    rhi::BindGroupEntry sampler;
    sampler.binding = 1;
    sampler.sampler = sampler_->handle();
    set_ = std::make_unique<rhi::BindGroup>(*layout_, std::vector<rhi::BindGroupEntry>{image, sampler, buffer});
#else
    image.sampler = sampler_->handle();
    set_ = std::make_unique<rhi::BindGroup>(*layout_, std::vector<rhi::BindGroupEntry>{image, buffer});
#endif
}

AutoExposurePass::PushConstants AutoExposurePass::pushConstants(const SceneSettings& settings,
                                                                const glm::vec4& sourceRect,
                                                                float deltaSeconds) {
    PushConstants push;
    push.sourceRect = sourceRect;
    const float low = std::min(settings.autoExposureMin, settings.autoExposureMax);
    const float high = std::max(settings.autoExposureMin, settings.autoExposureMax);
    // Exponential approach: the same share of the way each second whatever
    // the frame rate.
    const float speed = std::max(settings.autoExposureSpeed, 0.0f);
    const float share = 1.0f - std::exp(-speed * std::max(deltaSeconds, 0.0f));
    push.range = glm::vec4(low, high, std::log2(std::max(settings.exposureKey, 1e-4f)), share);
    return push;
}

void AutoExposurePass::record(rhi::CommandEncoder& encoder, const SceneSettings& settings,
                              const glm::vec4& sourceRect, float deltaSeconds) const {
    if (!set_) return;
    // The previous frame's tonemap read the state; this frame rewrites it.
    encoder.graphicsToComputeBarrier();
    const PushConstants push = pushConstants(settings, sourceRect, deltaSeconds);
    auto pass = encoder.beginComputePass();
    pass.setPipeline(*pipeline_);
    pass.setBindGroup(0, *set_);
    pass.setPushConstants(&push, sizeof(push));
    pass.dispatch(1);
    pass.end();
    encoder.computeToGraphicsBarrier();
}

} // namespace saida
