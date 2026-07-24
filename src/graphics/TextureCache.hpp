#pragma once

#include "project/AssetLoader.hpp"
#include "project/AssetRegistry.hpp"
#include "rhi/Format.hpp"
#include "rhi/Rhi.hpp"

#ifdef SAIDA_RHI_WEBGPU
#include "graphics/Texture.hpp"
#endif

#include <cstddef>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace saida {

class BindlessTables;
class GpuGraveyard;
#ifndef SAIDA_RHI_WEBGPU
class Texture;
#endif

// Owns cached and fallback textures, keeping pending/failed state, bindless
// registration, resident bytes, and LRU timestamps consistent for every entry.
class TextureCache {
public:
    struct EvictionCandidate {
        AssetID id = kAssetInvalid;
        uint64_t lastUse = 0;
    };

    TextureCache(rhi::Device& device, BindlessTables& bindlessTables);
    ~TextureCache();
    TextureCache(const TextureCache&) = delete;
    TextureCache& operator=(const TextureCache&) = delete;

    Texture* get(AssetID id, bool srgb, AssetRegistry* registry,
                 AssetLoader& loader, uint64_t frameClock);
    // Appends every completed ID (ready or failed) so the owner can rebind
    // materials without the cache depending on Material or ResourceManager.
    void finalizePending(std::vector<AssetID>& completed);

    AssetID registerMemory(const uint8_t* data, size_t size, bool srgb);
    AssetID registerGenerated(const uint8_t* pixels, uint32_t width,
                              uint32_t height, rhi::Format format,
                              bool generateMipmaps);

    Texture* defaultWhite();
    Texture* defaultNormal();
    Texture* missing();

    size_t size() const { return textures_.size(); }
    uint64_t residentBytes() const { return residentBytes_; }

    uint64_t sweepUnused(const std::unordered_set<AssetID>& live,
                         GpuGraveyard& graveyard, uint64_t frameClock);
    void collectEvictionCandidates(const std::unordered_set<AssetID>& live,
                                   std::vector<EvictionCandidate>& out) const;
    uint64_t evict(AssetID id, GpuGraveyard& graveyard, uint64_t frameClock);

    void clear();

private:
    struct PendingTexture {
        bool srgb = true;
        AssetHandle handle;
    };

    void ensureDefaultTextures();
    void registerBindless(Texture* texture);

    rhi::Device& device_;
    BindlessTables& bindlessTables_;
    std::unordered_map<AssetID, std::unique_ptr<Texture>> textures_;
    std::unordered_map<AssetID, PendingTexture> pending_;
    std::unordered_set<AssetID> failed_;
    std::unordered_map<AssetID, uint64_t> lastUse_;
    std::unique_ptr<Texture> defaultWhite_;
    std::unique_ptr<Texture> defaultNormal_;
    std::unique_ptr<Texture> missing_;
    uint64_t residentBytes_ = 0;
};

} // namespace saida
