#include "render/PostProcessor.hpp"

#include "core/Paths.hpp"
#include "graphics/GpuProfiler.hpp"
#include "graphics/Pipeline.hpp"
#ifndef SAIDA_RHI_WEBGPU
#include "graphics/VulkanDevice.hpp"
#endif
#include "scene/Scene.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <stdexcept>

namespace saida {

namespace {
constexpr uint32_t kMaxBloomLevels = 6;
constexpr uint32_t kMinBloomSize = 16;
}

PostProcessor::PostProcessor(rhi::Device& device, rhi::Extent2D extent, rhi::Format hdrFormat,
                             rhi::TextureView hdrInputView)
    : device_(device), extent_(extent), hdrFormat_(hdrFormat), hdrInputView_(hdrInputView) {
    createTargets();
    createSampler();
    createDescriptorResources();
    createPipelines();
}

PostProcessor::~PostProcessor() {
    bloomUpsamplePipeline_.reset();
    bloomDownsamplePipeline_.reset();
    downsampleGroups_.clear();  // groups return their sets to the layout's pool
    upsampleGroups_.clear();
    inputLayout_.reset();
    linearSampler_.reset();
    destroyTargets();
}

void PostProcessor::setHdrInput(rhi::TextureView hdrInputView) {
    if (hdrInputView_ == hdrInputView) return;
    hdrInputView_ = hdrInputView;
    updateDescriptorSets();
}

rhi::TextureView PostProcessor::bloomView() const {
    return bloom_.empty() ? rhi::TextureView{} : bloom_.front().texture->view();
}

void PostProcessor::createTargets() {
    uint32_t w = std::max(1u, extent_.width / 2u);
    uint32_t h = std::max(1u, extent_.height / 2u);

    auto makeTarget = [&](uint32_t width, uint32_t height) {
        Target target{};
        target.extent = {width, height};
        rhi::RenderTextureDesc desc;
        desc.format = hdrFormat_;
        desc.width = width;
        desc.height = height;
        desc.usage = rhi::TextureUsage::ColorAttachment | rhi::TextureUsage::Sampled;
        desc.memoryCategory = "RenderTarget/PostBloom";
        target.texture = std::make_unique<rhi::RenderTexture>(device_, desc);
        return target;
    };

    while (w >= kMinBloomSize && h >= kMinBloomSize && bloom_.size() < kMaxBloomLevels) {
        bloom_.push_back(makeTarget(w, h));
        w /= 2u;
        h /= 2u;
    }

    if (bloom_.empty()) bloom_.push_back(makeTarget(1, 1));
}

void PostProcessor::destroyTargets() {
    bloom_.clear();
}

void PostProcessor::createSampler() {
    rhi::SamplerDesc desc;
    desc.mipFilter = rhi::FilterMode::Linear;
    linearSampler_ = std::make_unique<rhi::Sampler>(device_, desc);
}

void PostProcessor::createDescriptorResources() {
#ifdef SAIDA_RHI_WEBGPU
    inputLayout_ = std::make_unique<rhi::BindGroupLayout>(device_,
        std::vector<rhi::webgpu::BindGroupLayoutEntry>{
            {0, rhi::BindingType::SampledTexture, rhi::ShaderStages::Fragment},
            {1, rhi::BindingType::Sampler, rhi::ShaderStages::Fragment},
        });
#else
    inputLayout_ = std::make_unique<rhi::BindGroupLayout>(device_,
        std::vector<rhi::BindGroupLayoutEntry>{
            {0, rhi::BindingType::CombinedImageSampler, rhi::ShaderStages::Fragment},
        });
#endif
    updateDescriptorSets();
}

void PostProcessor::updateDescriptorSets() {
    if (!inputLayout_ || bloom_.empty() || !linearSampler_) return;

    // Bind groups are immutable: rebuild them against the current inputs. Callers
    // only re-point inputs while the GPU is idle (HDR/target recreation), the
    // same contract the old vkUpdateDescriptorSets path relied on.
    downsampleGroups_.clear();
    upsampleGroups_.clear();

    auto makeGroup = [&](rhi::TextureView view) {
        rhi::BindGroupEntry entry;
        entry.binding = 0;
        entry.view = view;
#ifdef SAIDA_RHI_WEBGPU
        rhi::BindGroupEntry samplerEntry;
        samplerEntry.binding = 1;
        samplerEntry.sampler = linearSampler_->handle();
        return std::make_unique<rhi::BindGroup>(*inputLayout_,
                                                std::vector<rhi::BindGroupEntry>{entry, samplerEntry});
#else
        entry.sampler = linearSampler_->handle();
        return std::make_unique<rhi::BindGroup>(*inputLayout_,
                                                std::vector<rhi::BindGroupEntry>{entry});
#endif
    };

    for (size_t i = 0; i < bloom_.size(); ++i) {
        downsampleGroups_.push_back(makeGroup(i == 0 ? hdrInputView_ : bloom_[i - 1].texture->view()));
    }
    for (size_t i = 0; i < bloom_.size(); ++i) {
        upsampleGroups_.push_back(makeGroup(bloom_[std::min(i + 1, bloom_.size() - 1)].texture->view()));
    }
}

void PostProcessor::createPipelines() {
    Pipeline::Desc desc;
#ifdef SAIDA_RHI_WEBGPU
    desc.vertPath = "/shaders/tonemap.vert.wgsl";
#else
    desc.vertPath = shaderPath("tonemap.vert.spv");
#endif
    desc.colorFormats = {hdrFormat_};
    desc.bindGroupLayouts = {inputLayout_.get()};
    desc.vertexInput = false;
    desc.depthTest = false;
    desc.depthWrite = false;
    desc.cullMode = rhi::CullMode::None;
    desc.pushConstantSize = sizeof(BloomPush);

#ifdef SAIDA_RHI_WEBGPU
    desc.fragPath = "/shaders/bloom_downsample.frag.wgsl";
#else
    desc.fragPath = shaderPath("bloom_downsample.frag.spv");
#endif
    desc.blendMode = rhi::BlendMode::None;
    bloomDownsamplePipeline_ = std::make_unique<Pipeline>(device_, desc);

#ifdef SAIDA_RHI_WEBGPU
    desc.fragPath = "/shaders/bloom_upsample.frag.wgsl";
#else
    desc.fragPath = shaderPath("bloom_upsample.frag.spv");
#endif
    desc.blendMode = rhi::BlendMode::Additive;
    bloomUpsamplePipeline_ = std::make_unique<Pipeline>(device_, desc);
}

void PostProcessor::transitionTarget(rhi::CommandEncoder& encoder, Target& target,
                                     rhi::ResourceState to) {
    if (target.state == to) return;
    encoder.transition(target.texture->image(), target.state, to);
    target.state = to;
}

rhi::RenderPassEncoder PostProcessor::beginFullscreenPass(rhi::CommandEncoder& encoder,
                                                          Target& target, bool clear) {
    rhi::RenderPassDesc pass;
    pass.colorCount = 1;
    pass.colors[0].view = target.texture->view();
    pass.colors[0].loadOp = clear ? rhi::LoadOp::Clear : rhi::LoadOp::Load;
    pass.width = target.extent.width;
    pass.height = target.extent.height;
    return encoder.beginRenderPass(pass);
}

void PostProcessor::recordBloom(rhi::CommandEncoder& encoder, const SceneSettings& settings,
                                const glm::vec4& sourceRect, GpuProfiler* profiler) {
    if (bloom_.empty()) return;

    const bool enabled = settings.bloomEnabled && settings.bloomIntensity > 0.0f && settings.bloomRadius > 0.0f;

    {
        SAIDA_GPU_PROFILE_SCOPE(profiler, encoder.handle(), "Post/BloomDownsample");
        for (size_t i = 0; i < bloom_.size(); ++i) {
            Target& target = bloom_[i];
            transitionTarget(encoder, target, rhi::ResourceState::ColorAttachment);
            rhi::RenderPassEncoder rp = beginFullscreenPass(encoder, target, true);
            rp.setPipeline(*bloomDownsamplePipeline_);
            rp.setBindGroup(0, *downsampleGroups_[i]);
            BloomPush push{};
            push.sourceRect = (i == 0) ? sourceRect : glm::vec4(0.0f, 0.0f, 1.0f, 1.0f);
            push.params = glm::vec4(std::max(settings.bloomThreshold, 0.0f),
                                    (enabled && i == 0) ? 1.0f : 0.0f,
                                    std::max(settings.bloomRadius, 0.0f),
                                    enabled ? 1.0f : 0.0f);
            rp.setPushConstants(&push, sizeof(push));
            rp.draw(3);
            rp.end();
            transitionTarget(encoder, target, rhi::ResourceState::ShaderRead);
        }
    }

    if (!enabled || bloom_.size() < 2) return;

    {
        SAIDA_GPU_PROFILE_SCOPE(profiler, encoder.handle(), "Post/BloomUpsample");
        for (size_t reverse = bloom_.size() - 1; reverse > 0; --reverse) {
            size_t targetIndex = reverse - 1;
            Target& target = bloom_[targetIndex];
            transitionTarget(encoder, target, rhi::ResourceState::ColorAttachment);
            rhi::RenderPassEncoder rp = beginFullscreenPass(encoder, target, false);
            rp.setPipeline(*bloomUpsamplePipeline_);
            rp.setBindGroup(0, *upsampleGroups_[targetIndex]);
            BloomPush push{};
            push.sourceRect = glm::vec4(0.0f, 0.0f, 1.0f, 1.0f);
            push.params = glm::vec4(0.0f, 0.0f, std::max(settings.bloomRadius, 0.0f), 0.0f);
            rp.setPushConstants(&push, sizeof(push));
            rp.draw(3);
            rp.end();
            transitionTarget(encoder, target, rhi::ResourceState::ShaderRead);
        }
    }
}

} // namespace saida
