#include "render/AmbientOcclusionPass.hpp"

#include "core/Paths.hpp"
#include "graphics/Pipeline.hpp"
#ifndef SAIDA_RHI_WEBGPU
#include "graphics/VulkanDevice.hpp"
#endif
#include "render/InverseProjection.hpp"
#include "scene/Scene.hpp"

#include <algorithm>
#include <string>
#include <vector>

namespace saida {

namespace {
// A zero exponent would make the AO term constant; clamping keeps a
// hand-edited scene from silently disabling the effect it asked for.
constexpr float kMinAoPower = 0.001f;

// `textures` sampled textures in the fragment stage, at bindings 0, 2, ...
// with their samplers at 1, 3, ... on the Web (ambient_occlusion_pass.glsl).
std::unique_ptr<rhi::BindGroupLayout> makeLayout(rhi::Device& device, uint32_t textures) {
#ifdef SAIDA_RHI_WEBGPU
    using WE = rhi::webgpu::BindGroupLayoutEntry;
    std::vector<WE> entries;
    for (uint32_t i = 0; i < textures; ++i) {
        WE e{};
        e.binding = 2 * i; e.type = rhi::BindingType::SampledTexture; e.visibility = rhi::ShaderStages::Fragment;
        e.unfilterable = i == 0;  // depth
        entries.push_back(e);
        e = {}; e.binding = 2 * i + 1; e.type = rhi::BindingType::Sampler;
        e.visibility = rhi::ShaderStages::Fragment;
        e.nonFilteringSampler = true;
        entries.push_back(e);
    }
    return std::make_unique<rhi::BindGroupLayout>(device, entries);
#else
    std::vector<rhi::BindGroupLayoutEntry> entries;
    for (uint32_t i = 0; i < textures; ++i)
        entries.push_back({2 * i, rhi::BindingType::CombinedImageSampler, rhi::ShaderStages::Fragment});
    return std::make_unique<rhi::BindGroupLayout>(device, entries);
#endif
}

std::unique_ptr<rhi::Pipeline> makePipeline(rhi::Device& device, const std::string& fragment,
                                            const rhi::BindGroupLayout& layout) {
    rhi::Pipeline::Desc desc;
    desc.vertPath = shaderPath("fullscreen.vert.spv");
    desc.fragPath = shaderPath(fragment);
    desc.colorFormats = {AmbientOcclusionPass::kFormat};
    desc.bindGroupLayouts = {&layout};
    desc.vertexInput = false;
    desc.depthTest = false;
    desc.depthWrite = false;
    // A fullscreen triangle is never culled: on the Web, naga's clip-space
    // flip reverses its winding (TonemapPass).
    desc.cullMode = rhi::CullMode::None;
    desc.pushConstantSize = sizeof(AmbientOcclusionPass::PushConstants);
    return std::make_unique<rhi::Pipeline>(device, desc);
}
} // namespace

AmbientOcclusionPass::AmbientOcclusionPass(rhi::Device& device) : device_(device) {
    kernelLayout_ = makeLayout(device_, 1);
    denoiseLayout_ = makeLayout(device_, 2);
    rhi::SamplerDesc nearest;
    nearest.magFilter = rhi::FilterMode::Nearest;
    nearest.minFilter = rhi::FilterMode::Nearest;
    sampler_ = std::make_unique<rhi::Sampler>(device_, nearest);
    kernelPipeline_ = makePipeline(device_, "ambient_occlusion.frag.spv", *kernelLayout_);
    denoisePipeline_ = makePipeline(device_, "ambient_occlusion_denoise.frag.spv", *denoiseLayout_);
}

AmbientOcclusionPass::~AmbientOcclusionPass() = default;

bool AmbientOcclusionPass::active(const SceneSettings& settings) {
    return settings.aoEnabled && settings.aoRadius > 0.0f && settings.aoIntensity > 0.0f;
}

AmbientOcclusionPass::Inputs AmbientOcclusionPass::makeInputs(rhi::TextureView depth,
                                                              rhi::TextureView raw) const {
    std::vector<rhi::BindGroupEntry> entries;
    for (rhi::TextureView view : {depth, raw}) {
        const uint32_t texture = static_cast<uint32_t>(entries.size());
        rhi::BindGroupEntry entry;
        entry.view = view;
#ifdef SAIDA_RHI_WEBGPU
        entry.binding = 2 * texture;
        entries.push_back(entry);
        rhi::BindGroupEntry samplerEntry;
        samplerEntry.binding = 2 * texture + 1;
        samplerEntry.sampler = sampler_->handle();
        entries.push_back(samplerEntry);
#else
        entry.binding = 2 * texture;
        entry.sampler = sampler_->handle();
        entries.push_back(entry);
#endif
    }
#ifdef SAIDA_RHI_WEBGPU
    const size_t perTexture = 2;
#else
    const size_t perTexture = 1;
#endif
    Inputs inputs;
    inputs.kernel = std::make_unique<rhi::BindGroup>(*kernelLayout_,
        std::vector<rhi::BindGroupEntry>(entries.begin(), entries.begin() + perTexture));
    inputs.denoise = std::make_unique<rhi::BindGroup>(*denoiseLayout_, entries);
    return inputs;
}

AmbientOcclusionPass::PushConstants AmbientOcclusionPass::pushConstants(
        const SceneSettings& settings, const glm::mat4& projection, const glm::vec4& sourceRect) {
    PushConstants push;
    push.settings = glm::vec4(active(settings) ? 1.0f : 0.0f, std::max(settings.aoRadius, 0.0f),
                              std::max(settings.aoIntensity, 0.0f),
                              std::max(settings.aoPower, kMinAoPower));
    const InverseProjection inverse = inverseProjection(projection);
    push.invProjection = inverse.scale;
    push.invProjection2 = inverse.depth;
    push.invOffsets = inverse.offsets;
    push.sourceRect = sourceRect;
    return push;
}

void AmbientOcclusionPass::record(rhi::RenderPassEncoder& rp, Stage stage, const Inputs& inputs,
                                  const SceneSettings& settings, const glm::mat4& projection,
                                  const rhi::Rect2D& renderRect, const glm::vec4& sourceRect) const {
    const bool kernel = stage == Stage::Kernel;
    rp.setPipeline(kernel ? *kernelPipeline_ : *denoisePipeline_);
    rp.setViewport(static_cast<float>(renderRect.offset.x), static_cast<float>(renderRect.offset.y),
                   static_cast<float>(renderRect.extent.width),
                   static_cast<float>(renderRect.extent.height));
    rp.setScissor(renderRect.offset.x, renderRect.offset.y,
                  renderRect.extent.width, renderRect.extent.height);
    rp.setBindGroup(0, kernel ? *inputs.kernel : *inputs.denoise);
    const PushConstants push = pushConstants(settings, projection, sourceRect);
    rp.setPushConstants(&push, sizeof(PushConstants));
    rp.draw(3);
}

} // namespace saida
