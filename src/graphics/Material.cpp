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
    float _pad;
    glm::vec4 emissive;
};
}

Material::Material(rhi::Device& device, ResourceManager& manager, const MaterialDesc& desc)
    : device_(device), desc_(desc) {

    MaterialParams params{desc.baseColor, desc.metallic, desc.roughness, desc.ao, 0.0f, desc.emissiveColor};
    paramsBuffer_ = std::make_unique<Buffer>(device_, sizeof(MaterialParams),
        rhi::BufferUsage::Uniform, MemoryUsage::HostVisible);
    paramsBuffer_->write(&params, sizeof(params));

    bindTextures(manager);

    // 2. GPU-Driven Path: Register into global MaterialData SSBO
    if (device_.capabilities().descriptorIndexing) {
        bindlessIndex_ = manager.registerMaterialData(
            desc.baseColor, desc.emissiveColor, desc.metallic, desc.roughness, desc.ao,
            manager.ensureBindlessTextureIndex(albedo_),
            manager.ensureBindlessTextureIndex(normalMap_),
            manager.ensureBindlessTextureIndex(metallicRoughnessMap_),
            manager.ensureBindlessTextureIndex(emissiveMap_),
            desc.type
        );
    }
}

void Material::rebindTextures(ResourceManager& manager) {
    // The old descriptor set may still be read by an in-flight frame: it
    // goes to the manager's graveyard instead of being destroyed here.
    manager.retireBindGroup(std::move(descriptorSet_));
    bindTextures(manager);
    if (device_.capabilities().descriptorIndexing) {
        manager.updateMaterialData(bindlessIndex_,
            desc_.baseColor, desc_.emissiveColor, desc_.metallic, desc_.roughness, desc_.ao,
            manager.ensureBindlessTextureIndex(albedo_),
            manager.ensureBindlessTextureIndex(normalMap_),
            manager.ensureBindlessTextureIndex(metallicRoughnessMap_),
            manager.ensureBindlessTextureIndex(emissiveMap_),
            desc_.type);
    }
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
