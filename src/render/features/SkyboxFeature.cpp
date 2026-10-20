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
#include <cmath>
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
    desc.depthCompare = rhi::kDepthCloserOrEqual;
    desc.cullMode = rhi::CullMode::None;
    desc.pushConstantSize = pushSize;
    desc.viewMask = ctx.viewMask;
    pipeline_ = std::make_unique<Pipeline>(ctx.device, desc);

    if (!stereo_) {
        Pipeline::Desc probe;
        probe.vertPath = shaderPath("skybox.vert.spv");
        probe.fragPath = shaderPath("skyprobe.skybox.frag.spv");
        probe.colorFormats = {SkyProbe::kFormat};
        probe.bindGroupLayouts = {setLayout_.get()};
        probe.vertexInput = false;
        probe.depthTest = false;
        probe.depthWrite = false;
        probe.cullMode = rhi::CullMode::None;
        probe.pushConstantSize = sizeof(MonoPush);
        probePipeline_ = std::make_unique<Pipeline>(ctx.device, probe);
    }
}

bool SkyboxFeature::bindSky(const SceneSettings& settings, float& blend) {
    if (settings.skyboxTexture == kAssetInvalid) return false;
    Texture* tex = resources_->getTexture(settings.skyboxTexture);
    if (!tex) return false;

    // The second sky, when there is one and it has loaded. It is requested even
    // at a blend of zero so it is resident before a fade starts; until it has
    // loaded, the first sky is drawn alone rather than faded into nothing.
    Texture* blendTex = nullptr;
    if (settings.skyboxBlendTexture != kAssetInvalid)
        blendTex = resources_->getTexture(settings.skyboxBlendTexture);
    blend = blendTex ? std::clamp(settings.skyboxBlend, 0.0f, 1.0f) : 0.0f;
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

    return true;
}

SkyboxFeature::MonoPush SkyboxFeature::monoPush(const SceneSettings& settings,
                                                const glm::mat4& invViewProj, float blend) const {
    const float sunLength = glm::length(settings.skySunDirection);
    MonoPush pc{};
    pc.invViewProj = invViewProj;
    pc.exposure = settings.skyboxExposure;
    pc.rotation = settings.skyboxRotation;
    pc.blend = blend;
    pc.blendRotation = settings.skyboxBlendRotation;
    pc.sunDirection = sunLength > 0.0f
        ? glm::vec4(settings.skySunDirection / sunLength,
                    glm::radians(std::max(settings.skySunSize, 0.0f)))
        : glm::vec4(0.0f, 1.0f, 0.0f, 0.0f);
    const float stormCover = std::isfinite(settings.stormCloudCover)
        ? std::clamp(settings.stormCloudCover, 0.0f, 1.0f) : 0.0f;
    pc.sunColor = glm::vec4(glm::max(settings.skySunColor, glm::vec3(0.0f)), stormCover);
    return pc;
}

void SkyboxFeature::recordPrePass(const PrePassContext& pc) {
    SkyProbe* probe = pc.skyProbe;
    if (!probe || !probe->texture || !probePipeline_ || pc.stereo || !pc.camera) return;
    const auto& settings = pc.scene.settings();
    if (!(settings.fogEnabled && settings.fogSkyColor > 0.0f)) return;
    float blend = 0.0f;
    if (!bindSky(settings, blend)) return;

    if (probe->state != rhi::ResourceState::ColorAttachment) {
        pc.encoder.transition(probe->texture->image(), probe->state, rhi::ResourceState::ColorAttachment);
        probe->state = rhi::ResourceState::ColorAttachment;
    }
    rhi::RenderPassDesc pass;
    pass.colorCount = 1;
    pass.colors[0].view = probe->texture->view();
    pass.colors[0].loadOp = rhi::LoadOp::Clear;
    pass.width = SkyProbe::kWidth;
    pass.height = SkyProbe::kHeight;
    rhi::RenderPassEncoder rp = pc.encoder.beginRenderPass(pass);
    rp.setPipeline(*probePipeline_);
    rp.setBindGroup(0, *set_);
    // World from view: the probe is laid out around the camera's own axes.
    const glm::mat4 worldFromView = glm::mat4(glm::transpose(glm::mat3(pc.camera->view())));
    const MonoPush push = monoPush(settings, worldFromView, blend);
    rp.setPushConstants(&push, sizeof(MonoPush));
    rp.draw(3);
    rp.end();
    pc.encoder.transition(probe->texture->image(), rhi::ResourceState::ColorAttachment,
                          rhi::ResourceState::ShaderRead);
    probe->state = rhi::ResourceState::ShaderRead;
    probe->written = true;
}

void SkyboxFeature::record(FrameContext& fc) {
    if (fc.passthrough) return;  // XR see-through: an opaque sky would hide the world

    const auto& settings = fc.scene.settings();
    float blend = 0.0f;
    if (!bindSky(settings, blend)) return;

    fc.pass.setPipeline(*pipeline_);
    fc.pass.setBindGroup(0, *set_);

    const float sunLength = glm::length(settings.skySunDirection);
    const glm::vec4 sunDirection = sunLength > 0.0f
        ? glm::vec4(settings.skySunDirection / sunLength,
                    glm::radians(std::max(settings.skySunSize, 0.0f)))
        : glm::vec4(0.0f, 1.0f, 0.0f, 0.0f);
    const float stormCover = std::isfinite(settings.stormCloudCover)
        ? std::clamp(settings.stormCloudCover,0.0f,1.0f) : 0.0f;
    // Cloud cover shares the Sun's unused alpha lane.
    const glm::vec4 sunColor(glm::max(settings.skySunColor, glm::vec3(0.0f)), stormCover);
    const bool fogActive = settings.fogEnabled &&
        (settings.fogDensity > 0.0f || glm::any(glm::greaterThan(settings.fogRayleigh, glm::vec3(0.0f))));
    const glm::vec4 fogColor(glm::vec3(settings.fogColor), fogActive ? 1.0f : 0.0f);
    glm::dvec3 up(0.0, 1.0, 0.0);
    if (settings.fogPlanetRadius > 0.0f) {
        // XR has per-eye poses and deliberately no desktop camera.
        glm::dvec3 eye(0.0);
        if (fc.camera) eye = glm::dvec3(fc.camera->position);
        else if (fc.eyes && !fc.eyes->empty()) {
            for (const auto& view : *fc.eyes) eye += glm::dvec3(view.eyePosition);
            eye /= double(fc.eyes->size());
        }
        const glm::dvec3 radial = eye - glm::dvec3(settings.fogPlanetCentre);
        if (glm::length(radial) > 0.0) up = glm::normalize(radial);
    }
    const glm::vec4 fogUp(glm::vec3(up), 0.0f);

    if (!stereo_) {
        glm::mat4 view = fc.camera->view();
        view[3] = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);  // strip translation
        MonoPush pc = monoPush(settings, glm::inverse(fc.camera->projection() * view), blend);
        pc.fogColor = fogColor;
        pc.fogUp = fogUp;
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
        pc.fogColor = fogColor;
        pc.fogUp = fogUp;
        fc.pass.setPushConstants(&pc, sizeof(StereoPush));
    }

    fc.pass.draw(3);
}

} // namespace saida
