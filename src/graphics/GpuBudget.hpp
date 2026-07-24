#pragma once

#include "project/AssetRegistry.hpp"

#include <cstddef>
#include <cstdint>
#include <unordered_set>

namespace saida {

class GpuGraveyard;
class Mesh;
class MeshCache;
class TextureCache;

// Owns the GPU residency policy, frame clock, counters, and live-asset
// snapshot; it never owns resources and evicts only through concrete caches.
class GpuBudget {
public:
    struct TrimResult {
        size_t textures = 0;
        size_t meshes = 0;
    };

    GpuBudget(MeshCache& meshes, TextureCache& textures,
              GpuGraveyard& graveyard);

    uint64_t residentBytes() const;

    void setLimit(uint64_t bytes) { limitBytes_ = bytes; }
    uint64_t limit() const { return limitBytes_; }
    uint64_t evictedCount() const { return evictedCount_; }
    uint64_t evictedBytes() const { return evictedBytes_; }

    void advanceFrame() { ++frameClock_; }
    uint64_t frameClock() const { return frameClock_; }

    void setLiveUsage(std::unordered_set<const Mesh*> meshes,
                      std::unordered_set<AssetID> textures);
    TrimResult trimUnused(const std::unordered_set<const Mesh*>& liveMeshes,
                          const std::unordered_set<AssetID>& liveTextures);
    void enforce();

private:
    MeshCache& meshes_;
    TextureCache& textures_;
    GpuGraveyard& graveyard_;
    uint64_t frameClock_ = 0;
    uint64_t limitBytes_ = 512ull * 1024ull * 1024ull;
    uint64_t evictedCount_ = 0;
    uint64_t evictedBytes_ = 0;
    bool overBudgetWarned_ = false;
    std::unordered_set<const Mesh*> liveMeshes_;
    std::unordered_set<AssetID> liveTextures_;
    bool hasLiveUsage_ = false;
};

} // namespace saida
