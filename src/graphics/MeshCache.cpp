#include "graphics/MeshCache.hpp"

#include "core/Log.hpp"
#include "core/Profiler.hpp"
#include "graphics/GeometryRegistry.hpp"
#include "graphics/GpuGraveyard.hpp"
#include "graphics/Mesh.hpp"
#include "graphics/Primitives.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <string>
#include <utility>

namespace saida {

namespace {

// Parses OBJ data off the main thread. GeometryRegistry upload remains in
// MeshCache::pumpUploads so all GPU work stays on the main thread.
AssetDecoder makeMeshDecoder() {
    return [](std::vector<uint8_t>&& bytes, AssetDecodeResult& out, std::string& error) {
        auto data = std::make_shared<MeshData>();
        if (!Mesh::parseObjBytes(bytes.data(), bytes.size(), *data, error)) return false;
        if (data->vertices.empty() || data->indices.empty()) {
            error = "no usable geometry (corrupt or empty OBJ)";
            return false;
        }
        out.bytes = static_cast<uint64_t>(data->vertices.size()) * sizeof(Vertex) +
                    static_cast<uint64_t>(data->indices.size()) * 2 * sizeof(uint32_t) +
                    static_cast<uint64_t>(data->vertices.size()) * sizeof(glm::vec3);
        out.payload = prepareMesh(std::move(*data));
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

void MeshCache::finalizePending(AssetRegistry* /*registry*/) {
    for (auto it = pending_.begin(); it != pending_.end();) {
        const AssetLoadState state = it->second.state();
        if (state == AssetLoadState::Queued || state == AssetLoadState::Loading) {
            ++it;
            continue;
        }

        const AssetID id = it->first;
        if (state == AssetLoadState::Ready) {
            if (auto data = std::static_pointer_cast<PreparedMesh>(it->second.payload()))
                uploads_.push_back({id, std::move(data)});
        }
        // Releasing the handle also releases decoded CPU geometry. On failure
        // the stable proxy remains empty and drawing it stays a no-op.
        it = pending_.erase(it);
    }
}

AssetID MeshCache::queueMemory(std::shared_ptr<const PreparedMesh> data, const std::string& subPath,
                               AssetRegistry* registry) {
    static std::atomic<AssetID> dynamicId{0x5000000000000000ULL};
    if (!data || data->geometry.vertices.empty() || data->geometry.indices.empty()) return kAssetInvalid;
    const AssetID id = registry && !subPath.empty() ? registry->registerAsset(subPath, AssetType::Mesh) : dynamicId++;
    if (meshes_.count(id)) return id;
    auto mesh = std::make_unique<Mesh>(geometry_);
    reverseMap_[mesh.get()] = id;
    meshes_.emplace(id, std::move(mesh));
    uploads_.push_back({id, std::move(data)});
    return id;
}

void MeshCache::pumpUploads(size_t byteBudget, double timeBudgetMs) {
    SAIDA_PROFILE_SCOPE("Resource/StreamMeshUploads");
    while (!completing_.empty() && geometry_.uploadComplete(completing_.front().serial)) {
        const auto& ready = completing_.front();
        auto* mesh = meshes_.at(ready.id).get();
        mesh->finishUpload(*ready.data);
        residentBytes_ += mesh->gpuBytes();
        completing_.pop_front();
    }
    if (uploads_.empty() || !geometry_.canSubmitUploads()) return;
    const auto start = std::chrono::steady_clock::now();
    constexpr size_t kSliceBytes = 256 * 1024;
    std::vector<Completing> finished;
    geometry_.beginUploadBatch();
    while (!uploads_.empty() && byteBudget >= sizeof(Vertex)) {
        auto& upload = uploads_.front();
        auto* mesh = meshes_.at(upload.id).get();
        try {
            if (!upload.reserved) { mesh->beginUpload(*upload.data); upload.reserved = true; }
            const auto& geometry = upload.data->geometry;
            const size_t bytes = std::min(byteBudget, kSliceBytes);
            const size_t vertices = std::min(geometry.vertices.size() - upload.vertex, bytes / sizeof(Vertex));
            const size_t indices = std::min(geometry.indices.size() - upload.index,
                (bytes - vertices * sizeof(Vertex)) / sizeof(uint32_t));
            mesh->uploadRange(*upload.data, upload.vertex, vertices, upload.index, indices);
            upload.vertex += vertices; upload.index += indices;
            byteBudget -= vertices * sizeof(Vertex) + indices * sizeof(uint32_t);
            if (upload.vertex == geometry.vertices.size() && upload.index == geometry.indices.size()) {
                finished.push_back({upload.id, upload.data, 0});
                uploads_.pop_front();
            }
        } catch (const std::exception& e) {
            Log::error("MeshCache: streamed mesh ", upload.id, " failed: ", e.what());
            mesh->cancelUpload();
            uploads_.pop_front();
        }
        if (std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() >= timeBudgetMs) break;
    }
    const auto serial = geometry_.endUploadBatch();
    for (auto& ready : finished) { ready.serial = serial; completing_.push_back(std::move(ready)); }
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
    for (auto it = completing_.begin(); it != completing_.end();) {
        if (!live.count(meshes_.at(it->id).get())) {
            meshes_.at(it->id)->cancelUpload();
            it = completing_.erase(it);
        } else ++it;
    }
    for (auto it = uploads_.begin(); it != uploads_.end();) {
        if (!live.count(meshes_.at(it->id).get())) {
            meshes_.at(it->id)->cancelUpload();
            it = uploads_.erase(it);
        } else ++it;
    }
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
        if (id == kAssetBuiltinCube || pending_.count(id) || !mesh->loaded() || live.count(mesh.get()))
            continue;
        const auto use = lastUse_.find(id);
        out.push_back({id, use != lastUse_.end() ? use->second : 0});
    }
}

uint64_t MeshCache::evict(AssetID id, GpuGraveyard& graveyard, uint64_t frameClock) {
    const auto it = meshes_.find(id);
    if (it == meshes_.end() || id == kAssetBuiltinCube || pending_.count(id) || !it->second->loaded())
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
    completing_.clear();
    uploads_.clear();
    pending_.clear();
    lastUse_.clear();
    reverseMap_.clear();
    meshes_.clear();
    residentBytes_ = 0;
}

} // namespace saida
