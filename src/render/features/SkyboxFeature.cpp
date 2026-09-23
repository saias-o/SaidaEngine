#include "render/features/SkyboxFeature.hpp"

#include "core/Paths.hpp"
#include "core/Camera.hpp"
#ifndef SAIDA_RHI_WEBGPU
#include "graphics/VulkanDevice.hpp"
#include "rhi/vulkan/Format.hpp"
#endif
#include "graphics/ResourceManager.hpp"
#include "graphics/Texture.hpp"
#include "scene/Scene.hpp"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <utility>
#include <vector>

namespace saida {

SkyboxFeature::~SkyboxFeature() = default;

void SkyboxFeature::createPipelines(const RenderContext& ctx) {
    device_ = &ctx.device;
    resources_ = &ctx.resources;
    stereo_ = ctx.stereo();

#ifdef SAIDA_RHI_WEBGPU
    // Web has no combined image sampler: separate texture + sampler per sky,
    // mirroring web_compat.glsl's DECL_TEX2D(0, 0, 1, skyboxTex) and
    // DECL_TEX2D(0, 2, 3, blendTex).
    auto entry = [](uint32_t binding, rhi::BindingType type) {
        rhi::webgpu::BindGroupLayoutEntry e{};
        e.binding = binding;
        e.type = type;
        e.visibility = rhi::ShaderStages::Fragment;
        return e;
    };
    setLayout_ = std::make_unique<rhi::BindGroupLayout>(*device_,
        std::vector<rhi::webgpu::BindGroupLayoutEntry>{
            entry(0, rhi::BindingType::SampledTexture), entry(1, rhi::BindingType::Sampler),
            entry(2, rhi::BindingType::SampledTexture), entry(3, rhi::BindingType::Sampler),
        });
#else
    setLayout_ = std::make_unique<rhi::BindGroupLayout>(*device_,
        std::vector<rhi::BindGroupLayoutEntry>{
            {0, rhi::BindingType::CombinedImageSampler, rhi::ShaderStages::Fragment},
            {2, rhi::BindingType::CombinedImageSampler, rhi::ShaderStages::Fragment},
        });
#endif

    // Depth test on (LEQUAL — sky is at z=1), no depth write, two-sided.
    const char* frag = stereo_ ? "multiview.skybox.frag.spv" : "skybox.frag.spv";
    const uint32_t pushSize = stereo_ ? sizeof(StereoPush) : sizeof(MonoPush);
    Pipeline::Desc desc;
    desc.vertPath = shaderPath("skybox.vert.spv");
    desc.fragPath = shaderPath(frag);
    desc.colorFormats = {ctx.colorFormat};
    desc.depthFormat = ctx.depthFormat;
    desc.bindGroupLayouts = {setLayout_.get()};
    desc.samples = ctx.samples;
    desc.vertexInput = false;
    desc.depthWrite = false;
    desc.depthCompare = rhi::CompareOp::LessOrEqual;
    desc.cullMode = rhi::CullMode::None;
    desc.pushConstantSize = pushSize;
    desc.viewMask = ctx.viewMask;
    pipeline_ = std::make_unique<Pipeline>(ctx.device, desc);
}

void SkyboxFeature::record(FrameContext& fc) {
    if (fc.passthrough) return;  // XR see-through: an opaque sky would hide the world

    const auto& settings = fc.scene.settings();
    if (settings.skyboxTexture == kAssetInvalid) return;
    Texture* tex = resources_->getTexture(settings.skyboxTexture);
    if (!tex) return;

    // The second sky, when there is one and it has loaded. It is requested even
    // at a blend of zero so it is resident before a fade starts; until it has
    // loaded, the first sky is drawn alone rather than faded into nothing.
    Texture* blendTex = nullptr;
    if (settings.skyboxBlendTexture != kAssetInvalid)
        blendTex = resources_->getTexture(settings.skyboxBlendTexture);
    const float blend = blendTex ? std::clamp(settings.skyboxBlend, 0.0f, 1.0f) : 0.0f;
    // A layout slot is never left empty: with no second sky, the first fills it.
    Texture* boundBlend = blendTex ? blendTex : tex;
    const AssetID blendId = blendTex ? settings.skyboxBlendTexture : settings.skyboxTexture;

    if (settings.skyboxTexture != currentTexture_ || tex != currentTexturePtr_ ||
        blendId != currentBlend_ || boundBlend != currentBlendPtr_ || !set_) {
        std::vector<rhi::BindGroupEntry> entries;
        for (auto [binding, texture] : {std::pair<uint32_t, Texture*>{0, tex}, {2, boundBlend}}) {
#ifdef SAIDA_RHI_WEBGPU
            rhi::BindGroupEntry texEntry;
            texEntry.binding = binding;
            texEntry.view = texture->imageView();
            rhi::BindGroupEntry samplerEntry;
            samplerEntry.binding = binding + 1;
            samplerEntry.sampler = texture->sampler();
            entries.push_back(texEntry);
            entries.push_back(samplerEntry);
#else
            rhi::BindGroupEntry entry;
            entry.binding = binding;
            entry.view = texture->imageView();
            entry.sampler = texture->sampler();
            entries.push_back(entry);
#endif
        }
        set_ = std::make_unique<rhi::BindGroup>(*setLayout_, entries);
        currentTexture_ = settings.skyboxTexture;
        currentTexturePtr_ = tex;
        currentBlend_ = blendId;
        currentBlendPtr_ = boundBlend;
    }

    fc.pass.setPipeline(*pipeline_);
    fc.pass.setBindGroup(0, *set_);

    const float sunLength = glm::length(settings.skySunDirection);
    const glm::vec4 sunDirection = sunLength > 0.0f
        ? glm::vec4(settings.skySunDirection / sunLength,
                    glm::radians(std::max(settings.skySunSize, 0.0f)))
        : glm::vec4(0.0f, 1.0f, 0.0f, 0.0f);
    const glm::vec4 sunColor(glm::max(settings.skySunColor, glm::vec3(0.0f)), 0.0f);

    if (!stereo_) {
        glm::mat4 view = fc.camera->view();
        view[3] = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);  // strip translation
        MonoPush pc{};
        pc.invViewProj = glm::inverse(fc.camera->projection() * view);
        pc.exposure = settings.skyboxExposure;
        pc.rotation = settings.skyboxRotation;
        pc.blend = blend;
        pc.blendRotation = settings.skyboxBlendRotation;
        pc.sunDirection = sunDirection;
        pc.sunColor = sunColor;
        fc.pass.setPushConstants(&pc, sizeof(MonoPush));
    } else {
        const auto& eyes = *fc.eyes;
        StereoPush pc{};
        const uint32_t n = std::min<uint32_t>(static_cast<uint32_t>(eyes.size()), 2);
        for (uint32_t i = 0; i < n; ++i) {
            glm::mat4 view = eyes[i].view;
            view[3] = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
            pc.invViewProj[i] = glm::inverse(eyes[i].projection * view);
        }
        if (n == 1) pc.invViewProj[1] = pc.invViewProj[0];
        pc.exposure = settings.skyboxExposure;
        pc.rotation = settings.skyboxRotation;
        pc.blend = blend;
        pc.blendRotation = settings.skyboxBlendRotation;
        pc.sunDirection = sunDirection;
        pc.sunColor = sunColor;
        fc.pass.setPushConstants(&pc, sizeof(StereoPush));
    }

    fc.pass.draw(3);
}

} // namespace saida
