#include "graphics/ShadowMap.hpp"

#include "core/Paths.hpp"
#include "graphics/Mesh.hpp"

#ifndef SAIDA_RHI_WEBGPU
#include "graphics/VulkanDevice.hpp"
#include "rhi/vulkan/Format.hpp"
#endif

#include <glm/glm.hpp>

#include <stdexcept>

namespace saida {

ShadowMap::ShadowMap(rhi::Device& device, uint32_t initialResolution) : device_(device), resolution_(initialResolution) {
#ifdef SAIDA_RHI_WEBGPU
    format_ = rhi::Format::Depth32Float;
#else
    VkFormat vkFormat = device_.findSupportedFormat(
        {VK_FORMAT_D32_SFLOAT, VK_FORMAT_D16_UNORM},
        VK_IMAGE_TILING_OPTIMAL,
        VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT);
    format_ = vkFormat == VK_FORMAT_D32_SFLOAT ? rhi::Format::Depth32Float : rhi::Format::Depth16;
#endif

    createTexture();
    createSampler();
    createPipeline();
}

ShadowMap::~ShadowMap() = default;

void ShadowMap::createTexture() {
    rhi::RenderTextureDesc desc;
    desc.format = format_;
    desc.width = resolution_;
    desc.height = resolution_;
    desc.layers = kMaxShadows;
    desc.usage = rhi::TextureUsage::DepthAttachment | rhi::TextureUsage::Sampled;
    texture_ = std::make_unique<rhi::RenderTexture>(device_, desc);

    // Transition every layer to the sampled (read-only) layout up front, so even
    // layers never rendered into hold the layout the descriptor expects. Rendered
    // layers cycle attachment -> read-only each frame via record().
    device_.withSingleTimeEncoder([&](rhi::CommandEncoder& enc) {
        enc.transition(texture_->image(), rhi::ResourceState::Undefined,
                       rhi::ResourceState::DepthRead, 0, kMaxShadows);
    });
}

void ShadowMap::createSampler() {
    rhi::SamplerDesc desc;
    desc.addressMode = rhi::AddressMode::ClampToBorder;
    desc.whiteBorder = true;        // outside frustum = lit
    desc.compareEnabled = true;     // hardware PCF
    sampler_ = std::make_unique<rhi::Sampler>(device_, desc);
}

void ShadowMap::createPipeline() {
    // Depth-only: a single vertex stage, no fragment shader, no color attachment.
    // Push constant: mat4 mvp = lightViewProj * model (vertex stage only).
    rhi::Pipeline::Desc desc;
#ifdef SAIDA_RHI_WEBGPU
    desc.vertPath = "/shaders/shadow.vert.wgsl";
#else
    desc.vertPath = shaderPath("shadow.vert.spv");
#endif
    desc.depthFormat = format_;
    desc.pushConstantSize = sizeof(glm::mat4);
    desc.pushConstantStages = rhi::ShaderStages::Vertex;
    desc.depthBias = true;  // combat shadow acne
    desc.depthBiasConstant = 1.25f;
    desc.depthBiasSlope = 1.75f;
    pipeline_ = std::make_unique<rhi::Pipeline>(device_, desc);
}

void ShadowMap::record(rhi::CommandEncoder& encoder, int count, const DrawGeometryFn& drawGeometry) {
    if (count <= 0) return;
    if (count > static_cast<int>(kMaxShadows)) count = kMaxShadows;

    for (int i = 0; i < count; ++i) {
        // This layer is sampled (init / previous frame); move it to a depth
        // attachment. Contents are cleared by the pass, so discard them.
        encoder.transition(texture_->image(), rhi::ResourceState::DepthRead,
                           rhi::ResourceState::DepthWrite,
                           static_cast<uint32_t>(i), 1, /*discardContents=*/true);

        rhi::RenderPassDesc pass;
        pass.width = resolution_;
        pass.height = resolution_;
        pass.depth.view = texture_->layerView(i);
        pass.depth.loadOp = rhi::LoadOp::Clear;
        pass.depth.clearDepth = 1.0f;

        rhi::RenderPassEncoder rp = encoder.beginRenderPass(pass);
        rp.setPipeline(*pipeline_);
        drawGeometry(rp, i);
        rp.end();

        // Back to a sampled layout for the main pass / bake.
        encoder.transition(texture_->image(), rhi::ResourceState::DepthWrite,
                           rhi::ResourceState::DepthRead,
                           static_cast<uint32_t>(i), 1);
    }
}

bool ShadowMap::resize(uint32_t newResolution) {
    if (resolution_ == newResolution) return false;

    resolution_ = newResolution;

    // Wait for device to finish before destroying resources
    device_.waitIdle();

    // Recreate the texture (sampler and pipeline don't depend on resolution).
    texture_.reset();
    createTexture();

    return true;
}

} // namespace saida
