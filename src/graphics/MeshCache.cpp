#include "graphics/MeshCache.hpp"

#include "core/Log.hpp"
#include "core/Profiler.hpp"
#include "graphics/GeometryRegistry.hpp"
#include "graphics/GpuGraveyard.hpp"
#include "graphics/Mesh.hpp"
#include "graphics/Primitives.hpp"

#include <algorithm>
#include <atomic>
#include <string>
#include <utility>

namespace saida {

namespace {

// Parses OBJ data off the main thread. GeometryRegistry upload remains in
// MeshCache::finalizePending so all GPU work stays on the main thread.
AssetDecoder makeMeshDecoder() {
    return [](std::vector<uint8_t>&& bytes, AssetDecodeResult& out, std::string& error) {
        auto data = std::make_shared<MeshData>();
        if (!Mesh::parseObjBytes(bytes.data(), bytes.size(), *data, error)) return false;
        if (data->vertices.empty() || data->indices.empty()) {
            error = "no usable geometry (corrupt or empty OBJ)";
            return false;
        }
        out.payload = data;
        out.bytes = static_cast<uint64_t>(data->vertices.size()) * sizeof(Vertex) +
                    static_cast<uint64_t>(data->indices.size()) * sizeof(uint32_t);
        return true;
    };
}

} // namespace

MeshCache::MeshCache(GeometryRegistry& geometry) : geometry_(geometry) {}
MeshCache::~MeshCache() = default;

Mesh* MeshCache::create(AssetID id, const std::vector<Vertex>& vertices,
                        const std::vector<uint32_t>& indices) {
    // Never ask the backend to create an empty GPU buffer.
    if (vertices.empty() || indices.empty()) {
        Log::error("createMesh: refusing empty geometry for asset ", id);
        return nullptr;
    }

    auto mesh = std::make_unique<Mesh>(geometry_, vertices, indices);
    Mesh* ptr = mesh.get();
    residentBytes_ += ptr->gpuBytes();
    meshes_.emplace(id, std::move(mesh));
    reverseMap_[ptr] = id;
    return ptr;
}

Mesh* MeshCache::load(AssetID id, AssetRegistry* registry, AssetLoader& loader) {
    if (auto it = meshes_.find(id); it != meshes_.end())
        return it->second.get();

    if (!registry) return nullptr;
    const std::string path = registry->getPath(id);
    if (path.empty()) return nullptr;

    AssetHandle handle = loader.request(id, AssetLoadPriority::High,
                                        AssetPayloadKind::MeshObj, makeMeshDecoder());
    if (!handle) return nullptr;

    // The stable empty proxy lets nodes retain a Mesh* while CPU parsing runs.
    auto mesh = std::make_unique<Mesh>(geometry_);
    Mesh* ptr = mesh.get();
    meshes_.emplace(id, std::move(mesh));
    reverseMap_[ptr] = id;
    pending_.emplace(id, std::move(handle));
    return ptr;
}

Mesh* MeshCache::get(AssetID id, AssetRegistry* registry, AssetLoader& loader,
                     uint64_t frameClock) {
    if (auto it = meshes_.find(id); it != meshes_.end()) {
        lastUse_[id] = frameClock;
        return it->second.get();
    }
    if (id == kAssetBuiltinCube)
        return create(id, cubeVertices(), cubeIndices());
    return load(id, registry, loader);
}

void MeshCache::finalizePending(AssetRegistry* registry) {
    for (auto it = pending_.begin(); it != pending_.end();) {
        const AssetLoadState state = it->second.state();
        if (state == AssetLoadState::Queued || state == AssetLoadState::Loading) {
            ++it;
            continue;
        }

        const AssetID id = it->first;
        if (state == AssetLoadState::Ready) {
            if (auto data = std::static_pointer_cast<MeshData>(it->second.payload())) {
                SAIDA_PROFILE_SCOPE("Resource/FinalizeAsyncMesh");
                if (auto meshIt = meshes_.find(id); meshIt != meshes_.end()) {
                    meshIt->second->upload(data->vertices, data->indices);
                    residentBytes_ += meshIt->second->gpuBytes();
                    Log::info("loaded '", registry ? registry->getPath(id) : std::string(), "': ",
                              data->vertices.size(), " vertices, ",
                              data->indices.size() / 3, " triangles");
                }
            }
        }
        // Releasing the handle also releases decoded CPU geometry. On failure
        // the stable proxy remains empty and drawing it stays a no-op.
        it = pending_.erase(it);
    }
}

AssetID MeshCache::registerMemory(const std::vector<Vertex>& vertices,
                                  const std::vector<uint32_t>& indices) {
    static std::atomic<AssetID> s_dynamicId{0x4000000000000000ULL};
    const AssetID id = s_dynamicId++;
    create(id, vertices, indices);
    return id;
}

AssetID MeshCache::registerMemory(const std::string& subPath,
                                  const std::vector<Vertex>& vertices,
                                  const std::vector<uint32_t>& indices,
                                  AssetRegistry* registry) {
    // A stable sub-asset key must keep both its AssetID and its Mesh instance:
    // live MeshNode pointers would dangle if a re-import replaced the object.
    if (!registry) return registerMemory(vertices, indices);
    const AssetID id = registry->registerAsset(subPath, AssetType::Mesh);
    if (id == kAssetInvalid) return registerMemory(vertices, indices);
    if (meshes_.find(id) != meshes_.end()) return id;
    create(id, vertices, indices);
    return id;
}

AssetID MeshCache::idFor(const Mesh* mesh) const {
    const auto it = reverseMap_.find(mesh);
    return it != reverseMap_.end() ? it->second : kAssetInvalid;
}

uint64_t MeshCache::sweepUnused(const std::unordered_set<const Mesh*>& live,
                                GpuGraveyard& graveyard, uint64_t frameClock) {
    uint64_t retiredBytes = 0;
    for (auto it = meshes_.begin(); it != meshes_.end();) {
        if (it->first == kAssetBuiltinCube || live.count(it->second.get()) ||
            pending_.count(it->first)) {
            ++it;
            continue;
        }

        const uint64_t bytes = it->second->gpuBytes();
        retiredBytes += bytes;
        residentBytes_ -= std::min(residentBytes_, bytes);
        lastUse_.erase(it->first);
        reverseMap_.erase(it->second.get());
        Retired retired;
        retired.mesh = std::move(it->second);
        graveyard.retire(std::move(retired), frameClock);
        it = meshes_.erase(it);
    }
    return retiredBytes;
}

void MeshCache::collectEvictionCandidates(
    const std::unordered_set<const Mesh*>& live,
    std::vector<EvictionCandidate>& out) const {
    for (const auto& [id, mesh] : meshes_) {
        if (id == kAssetBuiltinCube || pending_.count(id) || live.count(mesh.get()))
            continue;
        const auto use = lastUse_.find(id);
        out.push_back({id, use != lastUse_.end() ? use->second : 0});
    }
}

uint64_t MeshCache::evict(AssetID id, GpuGraveyard& graveyard, uint64_t frameClock) {
    const auto it = meshes_.find(id);
    if (it == meshes_.end() || id == kAssetBuiltinCube || pending_.count(id))
        return 0;

    const uint64_t bytes = it->second->gpuBytes();
    residentBytes_ -= std::min(residentBytes_, bytes);
    lastUse_.erase(id);
    reverseMap_.erase(it->second.get());
    Retired retired;
    retired.mesh = std::move(it->second);
    meshes_.erase(it);
    graveyard.retire(std::move(retired), frameClock);
    return bytes;
}

void MeshCache::clear() {
    pending_.clear();
    lastUse_.clear();
    reverseMap_.clear();
    meshes_.clear();
    residentBytes_ = 0;
}

} // namespace saida
