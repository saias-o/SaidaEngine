#include "render/TonemapPass.hpp"

#include "core/Paths.hpp"
#include "graphics/Pipeline.hpp"
#include "graphics/VulkanDevice.hpp"
#include "scene/Scene.hpp"

#include <algorithm>
#include <vector>

namespace saida {

namespace {
// A zero exponent would make the AO term constant; clamping keeps a
// hand-edited scene from silently disabling the effect it asked for.
constexpr float kMinAoPower = 0.001f;
} // namespace

TonemapPass::TonemapPass(rhi::Device& device, rhi::Format outputFormat) : device_(device) {
#ifdef SAIDA_RHI_WEBGPU
    using WE = rhi::webgpu::BindGroupLayoutEntry;
    const auto F = rhi::ShaderStages::Fragment;
    std::vector<WE> entries;
    WE e{};
    e.binding = 0; e.type = rhi::BindingType::SampledTexture; e.visibility = F;
    entries.push_back(e);
    e = {}; e.binding = 1; e.type = rhi::BindingType::SampledTexture; e.visibility = F;
    e.unfilterable = true;
    entries.push_back(e);
    e = {}; e.binding = 2; e.type = rhi::BindingType::SampledTexture; e.visibility = F;
    entries.push_back(e);
    e = {}; e.binding = 3; e.type = rhi::BindingType::Sampler; e.visibility = F;
    entries.push_back(e);
    e = {}; e.binding = 4; e.type = rhi::BindingType::Sampler; e.visibility = F;
    e.nonFilteringSampler = true;
    entries.push_back(e);
    e = {}; e.binding = 5; e.type = rhi::BindingType::Sampler; e.visibility = F;
    entries.push_back(e);
    setLayout_ = std::make_unique<rhi::BindGroupLayout>(device_, entries);
#else
    setLayout_ = std::make_unique<rhi::BindGroupLayout>(device_,
        std::vector<rhi::BindGroupLayoutEntry>{
            {0, rhi::BindingType::CombinedImageSampler, rhi::ShaderStages::Fragment},  // HDR
            {1, rhi::BindingType::CombinedImageSampler, rhi::ShaderStages::Fragment},  // depth (AO)
            {2, rhi::BindingType::CombinedImageSampler, rhi::ShaderStages::Fragment},  // bloom
        });
#endif

    rhi::SamplerDesc linearDesc;
    linearDesc.mipFilter = rhi::FilterMode::Linear;
    colorSampler_ = std::make_unique<rhi::Sampler>(device_, linearDesc);

    rhi::SamplerDesc nearestDesc;
    nearestDesc.magFilter = rhi::FilterMode::Nearest;
    nearestDesc.minFilter = rhi::FilterMode::Nearest;
    depthSampler_ = std::make_unique<rhi::Sampler>(device_, nearestDesc);

    rhi::Pipeline::Desc desc;
    desc.vertPath = shaderPath("tonemap.vert.spv");
    desc.fragPath = shaderPath("tonemap.frag.spv");
    desc.colorFormats = {outputFormat};
    desc.bindGroupLayouts = {setLayout_.get()};
    desc.vertexInput = false;
    desc.depthTest = false;
    // Fullscreen triangle: never cull. On web the naga SPIR-V→WGSL pass flips
    // clip-space Y, which inverts the winding of raw-clip-coord triangles (the
    // scene is unaffected: its projection flip cancels naga's) — with the
    // default Back cull the tonemap triangle disappears entirely.
    desc.cullMode = rhi::CullMode::None;
    desc.pushConstantSize = sizeof(PushConstants);
    pipeline_ = std::make_unique<rhi::Pipeline>(device_, desc);
}

TonemapPass::~TonemapPass() = default;

void TonemapPass::setInputs(rhi::TextureView hdr, rhi::TextureView depth, rhi::TextureView bloom,
                            rhi::SamplerHandle bloomSampler) {
    if (!setLayout_ || !colorSampler_ || !depthSampler_) return;

    rhi::BindGroupEntry hdrEntry;
    hdrEntry.binding = 0;
    hdrEntry.view = hdr;
#ifndef SAIDA_RHI_WEBGPU
    hdrEntry.sampler = colorSampler_->handle();
#endif

    rhi::BindGroupEntry depthEntry;
    depthEntry.binding = 1;
    depthEntry.view = depth;
#ifndef SAIDA_RHI_WEBGPU
    depthEntry.sampler = depthSampler_->handle();
#endif

    rhi::BindGroupEntry bloomEntry;
    bloomEntry.binding = 2;
    bloomEntry.view = bloom;
#ifndef SAIDA_RHI_WEBGPU
    bloomEntry.sampler = bloomSampler;
#endif

#ifdef SAIDA_RHI_WEBGPU
    rhi::BindGroupEntry hdrSamplerEntry;
    hdrSamplerEntry.binding = 3;
    hdrSamplerEntry.sampler = colorSampler_->handle();

    rhi::BindGroupEntry depthSamplerEntry;
    depthSamplerEntry.binding = 4;
    depthSamplerEntry.sampler = depthSampler_->handle();

    rhi::BindGroupEntry bloomSamplerEntry;
    bloomSamplerEntry.binding = 5;
    bloomSamplerEntry.sampler = bloomSampler;

    set_ = std::make_unique<rhi::BindGroup>(*setLayout_,
        std::vector<rhi::BindGroupEntry>{hdrEntry, depthEntry, bloomEntry,
            hdrSamplerEntry, depthSamplerEntry, bloomSamplerEntry});
#else
    set_ = std::make_unique<rhi::BindGroup>(*setLayout_,
        std::vector<rhi::BindGroupEntry>{hdrEntry, depthEntry, bloomEntry});
#endif
}

TonemapPass::PushConstants TonemapPass::pushConstants(const SceneSettings& settings,
                                                      const glm::mat4& projection,
                                                      float exposure) {
    PushConstants push{};
    push.invProjection = glm::inverse(projection);
    push.aoParams = glm::vec4(settings.aoEnabled ? 1.0f : 0.0f,
                              std::max(settings.aoRadius, 0.0f),
                              std::max(settings.aoIntensity, 0.0f),
                              std::max(settings.aoPower, kMinAoPower));
    push.fogColor = settings.fogColor;
    push.fogParams = glm::vec4(settings.fogEnabled ? 1.0f : 0.0f,
                               std::max(settings.fogStart, 0.0f),
                               std::max(settings.fogDensity, 0.0f), exposure);
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

void TonemapPass::record(rhi::RenderPassEncoder& rp, const SceneSettings& settings,
                         const glm::mat4& projection, const rhi::Rect2D& renderRect,
                         const glm::vec4& sourceRect, float exposure) const {
    if (!ready()) return;
    rp.setPipeline(*pipeline_);
    // After setPipeline, as the pre-extraction code did — see the header.
    rp.setViewport(static_cast<float>(renderRect.offset.x), static_cast<float>(renderRect.offset.y),
                   static_cast<float>(renderRect.extent.width),
                   static_cast<float>(renderRect.extent.height));
    rp.setScissor(renderRect.offset.x, renderRect.offset.y,
                  renderRect.extent.width, renderRect.extent.height);
    rp.setBindGroup(0, *set_);
    PushConstants push = pushConstants(settings, projection, exposure);
    push.sourceRect = sourceRect;
    rp.setPushConstants(&push, sizeof(PushConstants));
    rp.draw(3);
}

} // namespace saida
