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

// Variation across a tiled surface, in its texture coordinates
// (shaders/surface_variation.glsl). The default leaves the material exactly as
// authored; nothing here changes its mean albedo.
struct SurfaceVariation {
    float warp = 0.0f;         // texture repeats a texel moves, over cells of two repeats
    float macroScale = 0.0f;   // macro-noise cells per texture-coordinate unit; 0 turns it off
    float macroAlbedo = 0.0f;  // albedo multiplied by 1 +/- this
    float macroNormal = 0.0f;  // slope the macro noise adds to the normal

    bool operator==(const SurfaceVariation& o) const {
        return warp == o.warp && macroScale == o.macroScale && macroAlbedo == o.macroAlbedo &&
               macroNormal == o.macroNormal;
    }
};

struct MaterialDesc {
    AssetID albedoId = kAssetInvalid;
    AssetID normalId = kAssetInvalid;
    AssetID metallicRoughnessId = kAssetInvalid;
    AssetID emissiveId = kAssetInvalid;
    // Height for parallax occlusion mapping: 1 the surface as drawn, 0 the
    // deepest point, `parallaxDepth` texture-coordinate units below it. Read
    // by the bindless scene path only; elsewhere the surface stays flat.
    AssetID heightId = kAssetInvalid;
    AssetID specularColorId = kAssetInvalid; // sRGB RGB, KHR_materials_specular
    AssetID specularStrengthId = kAssetInvalid; // linear alpha
    glm::vec3 dielectricF0{0.04f}; // IOR reflectance times specularColorFactor
    float specularStrength = 1.0f; // dielectric lobe only; metals are unaffected
    // Camera-facing XY quad, centered at its mesh origin. Atlas columns cover
    // a full azimuth turn; rows cover -90..90 degrees elevation (one = level).
    uint32_t billboardColumns = 0; // 0 keeps the authored mesh transform
    uint32_t billboardRows = 1;
    glm::vec4 baseColor{1.0f};
    glm::vec4 emissiveColor{0.0f};
    float metallic = 0.0f;
    float roughness = 0.5f;
    float ao = 1.0f;
    // Alpha test threshold. 0 means opaque: no sample can be below it, so the
    // discard never fires and the opaque path costs nothing. glTF's MASK mode
    // maps to its alphaCutoff; BLEND has no sorted pass yet and also arrives
    // here as a cutout rather than rendering as an opaque black rectangle.
    float alphaCutoff = 0.0f;
    // Scales the normal map's tangent-plane slope; 1 draws it as authored.
    float normalStrength = 1.0f;
    float parallaxDepth = 0.0f;
    // The environment's specular reflection on this material, as a multiple
    // of the scene's (SceneSettings::iblSpecularIntensity), drawn even where
    // the scene's image-based lighting is off: glazing shows the sky without
    // every other surface taking its light. Weighed by the material's own
    // roughness and Fresnel, and only where it is smooth enough to mirror
    // (lighting.glsl), so a rough wall beside the glass does not show it.
    float environmentReflection = 0.0f;
    SurfaceVariation variation;
    bool doubleSided = false;
    MaterialType type = MaterialType::Lit;

    bool operator==(const MaterialDesc& o) const {
        return albedoId == o.albedoId && normalId == o.normalId &&
               metallicRoughnessId == o.metallicRoughnessId && emissiveId == o.emissiveId &&
               heightId == o.heightId && parallaxDepth == o.parallaxDepth &&
               specularColorId == o.specularColorId && specularStrengthId == o.specularStrengthId &&
               dielectricF0 == o.dielectricF0 && specularStrength == o.specularStrength &&
               billboardColumns == o.billboardColumns && billboardRows == o.billboardRows &&
               environmentReflection == o.environmentReflection &&
               baseColor == o.baseColor && emissiveColor == o.emissiveColor &&
               metallic == o.metallic && roughness == o.roughness && ao == o.ao &&
               alphaCutoff == o.alphaCutoff && normalStrength == o.normalStrength &&
               variation == o.variation && doubleSided == o.doubleSided && type == o.type;
    }
};

// A material's textures as indices into the bindless texture array.
struct MaterialTextureSlots {
    uint32_t albedo = 0;
    uint32_t normal = 0;
    uint32_t metallicRoughness = 0;
    uint32_t emissive = 0;
    uint32_t height = 0;
    uint32_t specularColor = 0;
    uint32_t specularStrength = 0;
};

} // namespace saida

namespace std {
template <>
struct hash<saida::MaterialDesc> {
    size_t operator()(const saida::MaterialDesc& d) const {
        size_t h = 0;
        auto combine = [&h](size_t v) { h ^= v + 0x9e3779b9 + (h << 6) + (h >> 2); };
        combine(d.albedoId); combine(d.normalId); combine(d.metallicRoughnessId); combine(d.emissiveId);
        combine(d.heightId); combine(std::hash<float>()(d.parallaxDepth));
        combine(d.specularColorId); combine(d.specularStrengthId);
        combine(d.billboardColumns); combine(d.billboardRows);
        combine(std::hash<float>()(d.dielectricF0.r)); combine(std::hash<float>()(d.dielectricF0.g));
        combine(std::hash<float>()(d.dielectricF0.b)); combine(std::hash<float>()(d.specularStrength));
        combine(std::hash<float>()(d.environmentReflection));
        combine(std::hash<float>()(d.baseColor.r)); combine(std::hash<float>()(d.baseColor.g));
        combine(std::hash<float>()(d.baseColor.b)); combine(std::hash<float>()(d.baseColor.a));
        combine(std::hash<float>()(d.metallic)); combine(std::hash<float>()(d.roughness));
        combine(std::hash<float>()(d.alphaCutoff));
        combine(std::hash<float>()(d.normalStrength));
        combine(std::hash<float>()(d.variation.warp)); combine(std::hash<float>()(d.variation.macroScale));
        combine(std::hash<float>()(d.variation.macroAlbedo)); combine(std::hash<float>()(d.variation.macroNormal));
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
    MaterialTextureSlots textureSlots(ResourceManager& manager) const;

    rhi::Device& device_;
    MaterialDesc desc_;
    Texture* albedo_;
    Texture* normalMap_;
    Texture* metallicRoughnessMap_;
    Texture* emissiveMap_;
    Texture* heightMap_;
    Texture* specularColorMap_;
    Texture* specularStrengthMap_;
    std::unique_ptr<Buffer> paramsBuffer_;
    std::unique_ptr<rhi::BindGroup> descriptorSet_;
    uint32_t bindlessIndex_ = 0;
};

} // namespace saida
