#pragma once

#include "project/AssetLoader.hpp"
#include "project/AssetRegistry.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace saida {

class GeometryRegistry;
class GpuGraveyard;
class Mesh;
struct Vertex;

// Owns every cached mesh and keeps its pointer index, pending loads, resident
// byte count, and LRU timestamps consistent for the lifetime of each entry.
class MeshCache {
public:
    struct EvictionCandidate {
        AssetID id = kAssetInvalid;
        uint64_t lastUse = 0;
    };

    explicit MeshCache(GeometryRegistry& geometry);
    ~MeshCache();
    MeshCache(const MeshCache&) = delete;
    MeshCache& operator=(const MeshCache&) = delete;

    Mesh* get(AssetID id, AssetRegistry* registry, AssetLoader& loader, uint64_t frameClock);
    Mesh* load(AssetID id, AssetRegistry* registry, AssetLoader& loader);
    void finalizePending(AssetRegistry* registry);

    AssetID registerMemory(const std::vector<Vertex>& vertices,
                           const std::vector<uint32_t>& indices);
    AssetID registerMemory(const std::string& subPath,
                           const std::vector<Vertex>& vertices,
                           const std::vector<uint32_t>& indices,
                           AssetRegistry* registry);
    AssetID idFor(const Mesh* mesh) const;

    size_t size() const { return meshes_.size(); }
    uint64_t residentBytes() const { return residentBytes_; }

    // Removes non-live, non-builtin meshes. Returns the bytes retired.
    uint64_t sweepUnused(const std::unordered_set<const Mesh*>& live,
                         GpuGraveyard& graveyard, uint64_t frameClock);
    void collectEvictionCandidates(const std::unordered_set<const Mesh*>& live,
                                   std::vector<EvictionCandidate>& out) const;
    // Removes one candidate and retires it safely. Returns its resident bytes.
    uint64_t evict(AssetID id, GpuGraveyard& graveyard, uint64_t frameClock);

    void clear();

private:
    Mesh* create(AssetID id, const std::vector<Vertex>& vertices,
                 const std::vector<uint32_t>& indices);

    GeometryRegistry& geometry_;
    std::unordered_map<AssetID, std::unique_ptr<Mesh>> meshes_;
    std::unordered_map<const Mesh*, AssetID> reverseMap_;
    std::unordered_map<AssetID, AssetHandle> pending_;
    std::unordered_map<AssetID, uint64_t> lastUse_;
    uint64_t residentBytes_ = 0;
};

} // namespace saida
