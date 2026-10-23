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
                                            std::vector<const rhi::BindGroupLayout*> layouts,
                                            rhi::Format format = AmbientOcclusionPass::kFormat,
                                            uint32_t pushSize = sizeof(AmbientOcclusionPass::PushConstants)) {
    rhi::Pipeline::Desc desc;
    desc.vertPath = shaderPath("fullscreen.vert.spv");
    desc.fragPath = shaderPath(fragment);
    desc.colorFormats = {format};
    for (const rhi::BindGroupLayout* layout : layouts) desc.bindGroupLayouts.push_back(layout);
    desc.vertexInput = false;
    desc.depthTest = false;
    desc.depthWrite = false;
    // A fullscreen triangle is never culled: on the Web, naga's clip-space
    // flip reverses its winding (TonemapPass).
    desc.cullMode = rhi::CullMode::None;
    desc.pushConstantSize = pushSize;
    return std::make_unique<rhi::Pipeline>(device, desc);
}
} // namespace

AmbientOcclusionPass::AmbientOcclusionPass(rhi::Device& device, rhi::BindGroupLayout& globalLayout) {
    kernelLayout_ = makeLayout(device, 1);
    pairLayout_ = makeLayout(device, 2);
    upsampleLayout_ = makeLayout(device, 3);
    rhi::SamplerDesc nearest;
    nearest.magFilter = rhi::FilterMode::Nearest;
    nearest.minFilter = rhi::FilterMode::Nearest;
    sampler_ = std::make_unique<rhi::Sampler>(device, nearest);
    kernelPipeline_ = makePipeline(device, "ambient_occlusion.frag.spv", {kernelLayout_.get()});
    denoisePipeline_ = makePipeline(device, "ambient_occlusion_denoise.frag.spv", {pairLayout_.get()});
    upsamplePipeline_ = makePipeline(device, "ambient_occlusion_upsample.frag.spv", {upsampleLayout_.get()},
                                     kMapFormat);
    // Set 1's single texture is the depth, as the kernel's set 0 is.
    irradiancePipeline_ = makePipeline(device, "ambient_occlusion_gi.frag.spv",
                                       {&globalLayout, kernelLayout_.get()}, kMapFormat,
                                       sizeof(IrradiancePushConstants));
}

AmbientOcclusionPass::~AmbientOcclusionPass() = default;

bool AmbientOcclusionPass::active(const SceneSettings& settings) {
    return settings.aoEnabled && settings.aoRadius > 0.0f && settings.aoIntensity > 0.0f;
}

AmbientOcclusionPass::Inputs AmbientOcclusionPass::makeInputs(rhi::TextureView depth,
                                                              rhi::TextureView raw,
                                                              rhi::TextureView denoised,
                                                              rhi::TextureView irradiance) const {
    // Texture i of `views` at binding 2 i (its sampler at 2 i + 1 on the Web,
    // combined otherwise), as ambient_occlusion_pass.glsl declares them.
    auto group = [this](rhi::BindGroupLayout& layout, const std::vector<rhi::TextureView>& views) {
        std::vector<rhi::BindGroupEntry> entries;
        for (uint32_t i = 0; i < views.size(); ++i) {
            rhi::BindGroupEntry entry;
            entry.binding = 2 * i;
            entry.view = views[i];
#ifdef SAIDA_RHI_WEBGPU
            entries.push_back(entry);
            rhi::BindGroupEntry samplerEntry;
            samplerEntry.binding = 2 * i + 1;
            samplerEntry.sampler = sampler_->handle();
            entries.push_back(samplerEntry);
#else
            entry.sampler = sampler_->handle();
            entries.push_back(entry);
#endif
        }
        return std::make_unique<rhi::BindGroup>(layout, entries);
    };
    Inputs inputs;
    inputs.kernel = group(*kernelLayout_, {depth});
    inputs.denoise = group(*pairLayout_, {depth, raw});
    inputs.upsample = group(*upsampleLayout_, {depth, denoised, irradiance});
    return inputs;
}

rhi::Extent2D AmbientOcclusionPass::gridExtent(rhi::Extent2D full) {
    return {(full.width + kGridDivisor - 1) / kGridDivisor, (full.height + kGridDivisor - 1) / kGridDivisor};
}

rhi::Rect2D AmbientOcclusionPass::gridRect(const rhi::Rect2D& viewport) {
    rhi::Rect2D rect = viewport;
    rect.offset.x = viewport.offset.x / static_cast<int32_t>(kGridDivisor);
    rect.offset.y = viewport.offset.y / static_cast<int32_t>(kGridDivisor);
    rect.extent = gridExtent(viewport.extent);
    return rect;
}

rhi::Rect2D AmbientOcclusionPass::stageRect(Stage stage, const rhi::Rect2D& viewport) {
    return stage == Stage::Upsample ? viewport : gridRect(viewport);  // Irradiance: the grid
}

AmbientOcclusionPass::PushConstants AmbientOcclusionPass::pushConstants(
        const SceneSettings& settings, const glm::mat4& projection, const rhi::Rect2D& viewport,
        const glm::vec4& sourceRect) {
    PushConstants push;
    push.settings = glm::vec4(active(settings) ? 1.0f : 0.0f, std::max(settings.aoRadius, 0.0f),
                              std::max(settings.aoIntensity, 0.0f),
                              std::max(settings.aoPower, kMinAoPower));
    const InverseProjection inverse = inverseProjection(projection);
    push.invProjection = inverse.scale;
    push.invProjection2 = inverse.depth;
    push.invOffsets = inverse.offsets;
    push.sourceRect = sourceRect;
    const rhi::Rect2D grid = gridRect(viewport);
    push.aoGrid = glm::vec4(float(grid.offset.x), float(grid.offset.y),
                            float(grid.extent.width), float(grid.extent.height));
    return push;
}

void AmbientOcclusionPass::record(rhi::RenderPassEncoder& rp, Stage stage, const Inputs& inputs,
                                  const SceneSettings& settings, const glm::mat4& projection,
                                  const rhi::Rect2D& viewport, const glm::vec4& sourceRect,
                                  bool withIrradiance) const {
    switch (stage) {
        case Stage::Kernel:
            rp.setPipeline(*kernelPipeline_);
            rp.setBindGroup(0, *inputs.kernel);
            break;
        case Stage::Denoise:
            rp.setPipeline(*denoisePipeline_);
            rp.setBindGroup(0, *inputs.denoise);
            break;
        case Stage::Upsample:
            rp.setPipeline(*upsamplePipeline_);
            rp.setBindGroup(0, *inputs.upsample);
            break;
        case Stage::Irradiance:
            return;  // recordIrradiance: it needs the scene's set 0
    }
    const rhi::Rect2D rect = stageRect(stage, viewport);
    rp.setViewport(static_cast<float>(rect.offset.x), static_cast<float>(rect.offset.y),
                   static_cast<float>(rect.extent.width), static_cast<float>(rect.extent.height));
    rp.setScissor(rect.offset.x, rect.offset.y, rect.extent.width, rect.extent.height);
    PushConstants push = pushConstants(settings, projection, viewport, sourceRect);
    push.stages.x = withIrradiance ? 1.0f : 0.0f;
    rp.setPushConstants(&push, sizeof(PushConstants));
    rp.draw(3);
}

void AmbientOcclusionPass::recordIrradiance(rhi::RenderPassEncoder& rp, const Inputs& inputs,
                                            const rhi::BindGroup& global, const glm::mat4& projection,
                                            const glm::mat4& view, const rhi::Rect2D& viewport,
                                            const glm::vec4& sourceRect) const {
    rp.setPipeline(*irradiancePipeline_);
    rp.setBindGroup(0, global);
    rp.setBindGroup(1, *inputs.kernel);
    const rhi::Rect2D rect = gridRect(viewport);
    rp.setViewport(static_cast<float>(rect.offset.x), static_cast<float>(rect.offset.y),
                   static_cast<float>(rect.extent.width), static_cast<float>(rect.extent.height));
    rp.setScissor(rect.offset.x, rect.offset.y, rect.extent.width, rect.extent.height);
    IrradiancePushConstants push;
    const InverseProjection inverse = inverseProjection(projection);
    push.invProjection = inverse.scale;
    push.invProjection2 = inverse.depth;
    push.invOffsets = inverse.offsets;
    push.sourceRect = sourceRect;
    push.aoGrid = glm::vec4(float(rect.offset.x), float(rect.offset.y),
                            float(rect.extent.width), float(rect.extent.height));
    const glm::mat4 worldFromView = glm::inverse(view);
    for (int row = 0; row < 3; ++row)
        push.worldFromView[row] = glm::vec4(worldFromView[0][row], worldFromView[1][row],
                                            worldFromView[2][row], worldFromView[3][row]);
    rp.setPushConstants(&push, sizeof(IrradiancePushConstants));
    rp.draw(3);
}

} // namespace saida
