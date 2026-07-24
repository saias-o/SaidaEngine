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

namespace saida {

SkyboxFeature::~SkyboxFeature() = default;

void SkyboxFeature::createPipelines(const RenderContext& ctx) {
    device_ = &ctx.device;
    resources_ = &ctx.resources;
    stereo_ = ctx.stereo();

#ifdef SAIDA_RHI_WEBGPU
    // Web has no combined image sampler: separate texture (0) + sampler (1),
    // mirroring web_compat.glsl's DECL_TEX2D(0, 0, 1, skyboxTex).
    setLayout_ = std::make_unique<rhi::BindGroupLayout>(*device_,
        std::vector<rhi::webgpu::BindGroupLayoutEntry>{
            [] { rhi::webgpu::BindGroupLayoutEntry e{}; e.binding = 0;
                 e.type = rhi::BindingType::SampledTexture;
                 e.visibility = rhi::ShaderStages::Fragment; return e; }(),
            [] { rhi::webgpu::BindGroupLayoutEntry e{}; e.binding = 1;
                 e.type = rhi::BindingType::Sampler;
                 e.visibility = rhi::ShaderStages::Fragment; return e; }(),
        });
#else
    setLayout_ = std::make_unique<rhi::BindGroupLayout>(*device_,
        std::vector<rhi::BindGroupLayoutEntry>{
            {0, rhi::BindingType::CombinedImageSampler, rhi::ShaderStages::Fragment},
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

    if (settings.skyboxTexture != currentTexture_ || tex != currentTexturePtr_ || !set_) {
#ifdef SAIDA_RHI_WEBGPU
        rhi::BindGroupEntry texEntry;
        texEntry.binding = 0;
        texEntry.view = tex->imageView();
        rhi::BindGroupEntry samplerEntry;
        samplerEntry.binding = 1;
        samplerEntry.sampler = tex->sampler();
        set_ = std::make_unique<rhi::BindGroup>(*setLayout_,
            std::vector<rhi::BindGroupEntry>{texEntry, samplerEntry});
#else
        rhi::BindGroupEntry entry;
        entry.binding = 0;
        entry.view = tex->imageView();
        entry.sampler = tex->sampler();
        set_ = std::make_unique<rhi::BindGroup>(*setLayout_, std::vector<rhi::BindGroupEntry>{entry});
#endif
        currentTexture_ = settings.skyboxTexture;
        currentTexturePtr_ = tex;
    }

    fc.pass.setPipeline(*pipeline_);
    fc.pass.setBindGroup(0, *set_);

    if (!stereo_) {
        glm::mat4 view = fc.camera->view();
        view[3] = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);  // strip translation
        MonoPush pc{};
        pc.invViewProj = glm::inverse(fc.camera->projection() * view);
        pc.exposure = settings.skyboxExposure;
        pc.rotation = settings.skyboxRotation;
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
        fc.pass.setPushConstants(&pc, sizeof(StereoPush));
    }

    fc.pass.draw(3);
}

} // namespace saida
