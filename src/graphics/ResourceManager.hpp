#pragma once

#ifndef SAIDA_RHI_WEBGPU
#include <vulkan/vulkan.h>
#endif

#include <glm/glm.hpp>

#include <memory>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>

#include "project/AssetRegistry.hpp"
#include "project/AssetLoader.hpp"
#include "graphics/Material.hpp"
#include "graphics/GeometryRegistry.hpp"
#include "graphics/AsyncAssetCache.hpp"
#include "graphics/BindlessTables.hpp"
#include "graphics/GpuGraveyard.hpp"
#include "graphics/MeshCache.hpp"
#include "graphics/TextureCache.hpp"
#include "rhi/Rhi.hpp"

#ifdef SAIDA_RHI_WEBGPU
#include "graphics/Buffer.hpp"
#include "graphics/Texture.hpp"
#endif

namespace saida {

class VulkanDevice;
class Mesh;
#ifndef SAIDA_RHI_WEBGPU
class Texture;
#endif
struct Vertex;
class Rig;
class AnimationClip;
class ClipView;
class AnimGraphAsset;
class RigAsset;
class GpuBudget;

// Loads and caches GPU resources. Owns material set 1 layout/pool.
class ResourceManager {
public:
    static constexpr uint32_t kMaxBindlessTextures = 8192;
    static constexpr uint32_t kMaxBindlessMaterials = 4096;
    ResourceManager(rhi::Device& device, AssetRegistry* registry = nullptr);
    ~ResourceManager();
    ResourceManager(const ResourceManager&) = delete;
    ResourceManager& operator=(const ResourceManager&) = delete;

    rhi::Device& device() { return device_; }

    // Mesh by id: a built-in primitive or an .obj AssetID.
    Mesh* getMesh(AssetID id);
    // Texture by id, non-blocking: returns the
    // texture if it is resident, otherwise kicks off asynchronous loading
    // (read + stbi decode on the worker) and returns nullptr — the caller
    // falls back to its defaults and materials are rebound once the
    // texture becomes ready. A failed asset yields the "missing" texture
    // (magenta checkerboard), never a repeated nullptr.
    Texture* getTexture(AssetID id, bool srgb = true);
    Material* getMaterial(const MaterialDesc& desc);
    Rig* getRig(AssetID id);
    AnimationClip* getAnimation(AssetID id);

    // The id a mesh was loaded with (for serialization).
    AssetID meshId(const Mesh* mesh) const;

    // The id a clip was registered with — its sub-asset key ("model.glb#Run")
    // via the registry. kAssetInvalid for clips not registered here.
    AssetID animationId(const AnimationClip* clip) const;

    AssetRegistry* registry() const { return registry_; }

    // Direct .obj load (e.g. heavy models). getMesh() delegates here for paths.
    Mesh* loadMesh(AssetID id);

    // Register a path dynamically (e.g. for hardcoded demo scenes without pre-sync)
    AssetID getOrRegister(const std::string& path, AssetType type = AssetType::Unknown, bool srgb = true,
                          rhi::AddressMode address = rhi::AddressMode::Repeat);

    AssetID registerMemoryTexture(const uint8_t* data, size_t size, bool srgb = true,
                                  rhi::AddressMode address = rhi::AddressMode::Repeat);
    AssetID registerGeneratedTexture(const uint8_t* pixels, uint32_t width, uint32_t height,
                                     rhi::Format format = rhi::Format::RGBA8Srgb,
                                     bool generateMipmaps = true);

    // Register a dynamically generated mesh (e.g. from gltf primitive)
    AssetID registerMemoryMesh(const std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices);
    // Variant with stable identity keyed by sub-asset ("model.gltf#mesh2_prim0"),
    // idempotent like registerMemoryRig — re-importing yields the same id.
    AssetID registerMemoryMesh(const std::string& subPath, const std::vector<Vertex>& vertices,
                               const std::vector<uint32_t>& indices);

    // Register memory structures directly from loaders
    AssetID registerMemoryRig(const std::string& path, std::unique_ptr<Rig> rig);
    AssetID registerMemoryAnimation(const std::string& subPath, std::unique_ptr<AnimationClip> clip);

    // Standalone animation-authoring assets, cached by AssetID. These calls
    // register the identity and kick off an asynchronous request. The getter
    // stays null until Ready; read/parse errors are exposed via the load
    // state and diagnostics without blocking the consumer.
    AssetID loadRigAsset(const std::string& path);
    const RigAsset* getRigAsset(AssetID id) const;
    AssetLoadState rigAssetLoadState(AssetID id) const;
    std::string rigAssetLoadError(AssetID id) const;

    AssetID loadClipView(const std::string& path);
    const ClipView* getClipView(AssetID id) const;
    AssetLoadState clipViewLoadState(AssetID id) const;
    std::string clipViewLoadError(AssetID id) const;

    AssetID loadAnimGraph(const std::string& path);
    const AnimGraphAsset* getAnimGraph(AssetID id) const;
    AssetLoadState animGraphLoadState(AssetID id) const;
    std::string animGraphLoadError(AssetID id) const;

    Texture* defaultWhiteTexture();
    Texture* defaultNormalTexture();
    // Visible fallback for a missing/corrupt asset: 2x2 magenta checkerboard.
    Texture* missingTexture();

    rhi::BindGroupLayout& materialSetLayout() const { return *materialSetLayout_; }

    // Global bindless texture/material tables (owned by BindlessTables).
    // Pipelines choose the set index.
#ifndef SAIDA_RHI_WEBGPU
    VkDescriptorSetLayout globalMaterialSetLayout() const { return bindlessTables_.layout(); }
    VkDescriptorSet globalMaterialSet() const { return bindlessTables_.set(); }
#endif
    Buffer* globalMaterialBuffer() const { return bindlessTables_.materialBuffer(); }
    
    GeometryRegistry& geometry() { return *geometryRegistry_; }

    void setRegistry(AssetRegistry* registry);
    AssetRegistry* getRegistry() const { return registry_; }
    AssetLoader& assetLoader() { return *assetLoader_; }
    const AssetLoader& assetLoader() const { return *assetLoader_; }
    // Per-frame tick: advances the retention clock, drains the GPU
    // graveyard, finalizes ready async loads (GPU creation + material
    // rebinding), then pumps the AssetLoader.
    void pumpAssetLoads();

    // True when nothing is queued, in flight, or waiting to be finalized into a
    // cache. A deterministic frame capture counts frames only from this point:
    // an asset that lands on frame 3 in one run and frame 4 in the next changes
    // what frame 3 shows, which would make a golden image compare a race rather
    // than the change under test. Failed loads count as settled — they will
    // never arrive, and waiting for them would hang instead of reporting.
    bool assetLoadsSettled() const;

    // GPU bytes of resident asset-loaded resources (textures, meshes) —
    // leak diagnostics for the GPU resource accounting, exposed via assets.stats().
    uint64_t gpuResidentBytes() const;

    // GPU budget enforced WHILE a scene is running (not just at changeScene):
    // once over budget, assets that are neither referenced by the live scene
    // (see setLiveUsage) nor currently loading are evicted, least-recently-used
    // first. If the entire overage is referenced, nothing breaks: a single
    // warning reports it. 0 = unlimited.
    void setGpuBudget(uint64_t bytes);
    uint64_t gpuBudgetBytes() const;
    uint64_t gpuEvictedCount() const;
    uint64_t gpuEvictedBytes() const;


    // Set of resources still referenced by live scenes — built by the
    // SceneTree (walking the World) after a changeScene.
    struct AssetUsage {
        std::unordered_set<const Mesh*> meshes;
        std::unordered_set<AssetID> textures;
        std::unordered_set<const Material*> materials;
        // Animation: rigs and clips still held (raw pointers) by live
        // Animators. The ClipView/AnimGraph caches only have transient
        // consumers (rebind by path) and are swept entirely on trim.
        std::unordered_set<const Rig*> rigs;
        std::unordered_set<const AnimationClip*> animations;
    };

    // Evicts from the cache everything
    // that `used` no longer references (mark-and-sweep on scene change).
    // GPU objects go to the graveyard and are destroyed kRetireFrames later
    // (an in-flight frame may still read them); their bindless indices and
    // material slots are then recycled. Builtins, default textures and
    // proxies currently loading are exempt.
    void trimUnused(const AssetUsage& used);

    // Snapshot of live references (SceneTree, refreshed on every hierarchy
    // change) — the candidates for mid-scene budget eviction are exactly
    // the assets outside this set.
    void setLiveUsage(AssetUsage usage);

    // Hands off a GPU object that may still be referenced by an in-flight
    // frame; it will be destroyed after kRetireFrames pumps (ThumbnailCache pattern).
    void retireBindGroup(std::unique_ptr<rhi::BindGroup> group);

    // Rewrites the MaterialData slot of an already-registered material
    // (rebind after one of its textures finishes async loading).
    void updateMaterialData(uint32_t index, const glm::vec4& baseColor, const glm::vec4& emissive,
                            float metallic, float roughness, float ao,
                            uint32_t albedoIdx, uint32_t normalIdx, uint32_t mrIdx, uint32_t emissiveIdx,
                            MaterialType type, float alphaCutoff);

    // Register a texture in the bindless array if needed, returns its index.
    uint32_t ensureBindlessTextureIndex(Texture* texture);
    
    // Register material data in the global SSBO, returns its index.
    uint32_t registerMaterialData(const glm::vec4& baseColor, const glm::vec4& emissive,
                                  float metallic, float roughness, float ao,
                                  uint32_t albedoIdx, uint32_t normalIdx, uint32_t mrIdx, uint32_t emissiveIdx,
                                  MaterialType type, float alphaCutoff);

private:
    void finalizePendingAnimationAssets();
    void rebindMaterialsUsing(AssetID textureId);

    rhi::Device& device_;
    AssetRegistry* registry_;
    std::unique_ptr<rhi::BindGroupLayout> materialSetLayout_;

    // Global bindless descriptor tables (texture array + material SSBO).
    BindlessTables bindlessTables_;

    std::unique_ptr<GeometryRegistry> geometryRegistry_;
    std::unique_ptr<MeshCache> meshCache_;
    std::unique_ptr<TextureCache> textureCache_;

    std::unordered_map<MaterialDesc, std::unique_ptr<Material>> materials_;
    std::unordered_map<AssetID, std::unique_ptr<Rig>> rigs_;
    std::unordered_map<AssetID, std::unique_ptr<AnimationClip>> animations_;
    // Standalone authoring animation assets, cached by AssetID via the shared
    // async cache (identical load/get/state/error/finalize for all three).
    AsyncAssetCache<RigAsset> rigAssetCache_;
    AsyncAssetCache<ClipView> clipViewCache_;
    AsyncAssetCache<AnimGraphAsset> animGraphCache_;
    
    std::unique_ptr<AssetLoader> assetLoader_;

    // GPU objects retired but possibly still read by an in-flight frame:
    // destroyed (and their bindless slots recycled) after a delay (GpuGraveyard).
    GpuGraveyard graveyard_;
    std::unique_ptr<GpuBudget> gpuBudget_;
};

} // namespace saida
