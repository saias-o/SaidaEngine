#pragma once

#include <glm/glm.hpp>

#include "project/AssetRegistry.hpp"
#include "rhi/Rhi.hpp"

#ifdef SAIDA_RHI_WEBGPU
#include "graphics/Buffer.hpp"
#include "graphics/Texture.hpp"
#endif

#include <memory>
#include <string>
#include <functional>

namespace saida {

#ifndef SAIDA_RHI_WEBGPU
class Texture;
class Buffer;
#endif

class ResourceManager;

// Selects the scene fragment pipeline; Lit and Unlit share the vertex/layout path.
enum class MaterialType : uint32_t {
    Lit = 0,
    Unlit = 1,
    Count,
};

struct MaterialDesc {
    AssetID albedoId = kAssetInvalid;
    AssetID normalId = kAssetInvalid;
    AssetID metallicRoughnessId = kAssetInvalid;
    AssetID emissiveId = kAssetInvalid;
    glm::vec4 baseColor{1.0f};
    glm::vec4 emissiveColor{0.0f};
    float metallic = 0.0f;
    float roughness = 0.5f;
    float ao = 1.0f;
    bool doubleSided = false;
    MaterialType type = MaterialType::Lit;

    bool operator==(const MaterialDesc& o) const {
        return albedoId == o.albedoId && normalId == o.normalId &&
               metallicRoughnessId == o.metallicRoughnessId && emissiveId == o.emissiveId &&
               baseColor == o.baseColor && emissiveColor == o.emissiveColor &&
               metallic == o.metallic && roughness == o.roughness && ao == o.ao &&
               doubleSided == o.doubleSided && type == o.type;
    }
};

} // namespace saida

namespace std {
template <>
struct hash<saida::MaterialDesc> {
    size_t operator()(const saida::MaterialDesc& d) const {
        size_t h = 0;
        auto combine = [&h](size_t v) { h ^= v + 0x9e3779b9 + (h << 6) + (h >> 2); };
        combine(d.albedoId); combine(d.normalId); combine(d.metallicRoughnessId); combine(d.emissiveId);
        combine(std::hash<float>()(d.baseColor.r)); combine(std::hash<float>()(d.baseColor.g));
        combine(std::hash<float>()(d.baseColor.b)); combine(std::hash<float>()(d.baseColor.a));
        combine(std::hash<float>()(d.metallic)); combine(std::hash<float>()(d.roughness));
        combine(std::hash<bool>()(d.doubleSided));
        combine(std::hash<uint32_t>()(static_cast<uint32_t>(d.type)));
        return h;
    }
};
} // namespace std

namespace saida {

class Material {
public:
    Material(rhi::Device& device, ResourceManager& manager, const MaterialDesc& desc);
    ~Material();
    Material(const Material&) = delete;
    Material& operator=(const Material&) = delete;

    const rhi::BindGroup& descriptorSet() const { return *descriptorSet_; }
    uint32_t bindlessIndex() const { return bindlessIndex_; }
    const MaterialDesc& desc() const { return desc_; }
    Texture* texture() const { return albedo_; }

    // Re-resolves the desc's textures and rebuilds the descriptor set +
    // the MaterialData entry — called by the ResourceManager when an
    // asynchronous texture becomes ready (or fails -> "missing" checkerboard).
    // The old descriptor set goes to the graveyard: an in-flight frame may
    // still read it.
    void rebindTextures(ResourceManager& manager);

private:
    void bindTextures(ResourceManager& manager);

    rhi::Device& device_;
    MaterialDesc desc_;
    Texture* albedo_;
    Texture* normalMap_;
    Texture* metallicRoughnessMap_;
    Texture* emissiveMap_;
    std::unique_ptr<Buffer> paramsBuffer_;
    std::unique_ptr<rhi::BindGroup> descriptorSet_;
    uint32_t bindlessIndex_ = 0;
};

} // namespace saida
