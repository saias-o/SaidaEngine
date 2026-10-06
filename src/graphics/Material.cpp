#include "graphics/Material.hpp"

#include "graphics/Buffer.hpp"
#include "graphics/Texture.hpp"
#include "graphics/ResourceManager.hpp"

#ifndef SAIDA_RHI_WEBGPU
#include "graphics/VulkanDevice.hpp"
#endif

#include <array>
#include <stdexcept>

namespace saida {

namespace {
struct MaterialParams {
    glm::vec4 baseColor;
    float metallic;
    float roughness;
    float ao;
    float alphaCutoff;   // 0 = opaque; was padding, now carries the alpha test
    glm::vec4 emissive;
    glm::vec4 variation; // SurfaceVariation: warp, macroScale, macroAlbedo, macroNormal
    glm::vec4 detail;    // x normal strength, y environment reflection
};

MaterialParams paramsOf(const MaterialDesc& d) {
    return {d.baseColor, d.metallic, d.roughness, d.ao, d.alphaCutoff, d.emissiveColor,
            glm::vec4(d.variation.warp, d.variation.macroScale, d.variation.macroAlbedo, d.variation.macroNormal),
            glm::vec4(d.normalStrength, d.environmentReflection, 0.0f, 0.0f)};
}
}

Material::Material(rhi::Device& device, ResourceManager& manager, const MaterialDesc& desc)
    : device_(device), desc_(desc) {

    const MaterialParams params = paramsOf(desc);
    paramsBuffer_ = std::make_unique<Buffer>(device_, sizeof(MaterialParams),
        rhi::BufferUsage::Uniform, MemoryUsage::HostVisible);
    paramsBuffer_->write(&params, sizeof(params));

    bindTextures(manager);

    // 2. GPU-Driven Path: Register into global MaterialData SSBO
    if (device_.capabilities().descriptorIndexing) {
        bindlessIndex_ = manager.registerMaterialData(desc, textureSlots(manager));
    }
}

void Material::rebindTextures(ResourceManager& manager) {
    // The old descriptor set may still be read by an in-flight frame: it
    // goes to the manager's graveyard instead of being destroyed here.
    manager.retireBindGroup(std::move(descriptorSet_));
    bindTextures(manager);
    if (device_.capabilities().descriptorIndexing) {
        manager.updateMaterialData(bindlessIndex_, desc_, textureSlots(manager));
    }
}

MaterialTextureSlots Material::textureSlots(ResourceManager& manager) const {
    return {manager.ensureBindlessTextureIndex(albedo_), manager.ensureBindlessTextureIndex(normalMap_),
            manager.ensureBindlessTextureIndex(metallicRoughnessMap_),
            manager.ensureBindlessTextureIndex(emissiveMap_),
            manager.ensureBindlessTextureIndex(heightMap_)};
}

void Material::bindTextures(ResourceManager& manager) {
    const MaterialDesc& desc = desc_;

    albedo_ = manager.getTexture(desc.albedoId);
    if (!albedo_) albedo_ = manager.defaultWhiteTexture();

    normalMap_ = manager.getTexture(desc.normalId, false);
    if (!normalMap_) normalMap_ = manager.defaultNormalTexture();

    metallicRoughnessMap_ = manager.getTexture(desc.metallicRoughnessId, false);
    if (!metallicRoughnessMap_) metallicRoughnessMap_ = manager.defaultWhiteTexture();

    emissiveMap_ = manager.getTexture(desc.emissiveId);
    if (!emissiveMap_) emissiveMap_ = manager.defaultWhiteTexture();

    heightMap_ = manager.getTexture(desc.heightId, false);
    if (!heightMap_) heightMap_ = manager.defaultWhiteTexture();

    // 1. Classic Path: set 1 (albedo/normal/metallic-roughness/params/emissive).
    rhi::BindGroupEntry albedoEntry;
    albedoEntry.binding = 0;
    albedoEntry.view = albedo_->imageView();
    albedoEntry.sampler = albedo_->sampler();

    rhi::BindGroupEntry normalEntry;
    normalEntry.binding = 1;
    normalEntry.view = normalMap_->imageView();
    normalEntry.sampler = normalMap_->sampler();

    rhi::BindGroupEntry mrEntry;
    mrEntry.binding = 2;
    mrEntry.view = metallicRoughnessMap_->imageView();
    mrEntry.sampler = metallicRoughnessMap_->sampler();

    rhi::BindGroupEntry paramsEntry;
    paramsEntry.binding = 3;
    paramsEntry.buffer = paramsBuffer_.get();
    paramsEntry.range = sizeof(MaterialParams);

    rhi::BindGroupEntry emissiveEntry;
    emissiveEntry.binding = 4;
    emissiveEntry.view = emissiveMap_->imageView();
    emissiveEntry.sampler = emissiveMap_->sampler();

#ifdef SAIDA_RHI_WEBGPU
    rhi::BindGroupEntry albedoSamplerEntry;
    albedoSamplerEntry.binding = 5;
    albedoSamplerEntry.sampler = albedo_->sampler();
    albedoEntry.sampler = nullptr;

    rhi::BindGroupEntry normalSamplerEntry;
    normalSamplerEntry.binding = 6;
    normalSamplerEntry.sampler = normalMap_->sampler();
    normalEntry.sampler = nullptr;

    rhi::BindGroupEntry mrSamplerEntry;
    mrSamplerEntry.binding = 7;
    mrSamplerEntry.sampler = metallicRoughnessMap_->sampler();
    mrEntry.sampler = nullptr;

    rhi::BindGroupEntry emissiveSamplerEntry;
    emissiveSamplerEntry.binding = 8;
    emissiveSamplerEntry.sampler = emissiveMap_->sampler();
    emissiveEntry.sampler = nullptr;

    descriptorSet_ = std::make_unique<rhi::BindGroup>(manager.materialSetLayout(),
        std::vector<rhi::BindGroupEntry>{
            albedoEntry, normalEntry, mrEntry, paramsEntry, emissiveEntry,
            albedoSamplerEntry, normalSamplerEntry, mrSamplerEntry, emissiveSamplerEntry});
#else
    descriptorSet_ = std::make_unique<rhi::BindGroup>(manager.materialSetLayout(),
        std::vector<rhi::BindGroupEntry>{albedoEntry, normalEntry, mrEntry, paramsEntry, emissiveEntry});
#endif
}

Material::~Material() = default;  // paramsBuffer_ / descriptorSet_ RAII

} // namespace saida
