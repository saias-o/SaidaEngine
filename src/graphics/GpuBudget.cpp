#include "graphics/GpuBudget.hpp"

#include "core/Log.hpp"
#include "graphics/GpuGraveyard.hpp"
#include "graphics/MeshCache.hpp"
#include "graphics/TextureCache.hpp"

#include <algorithm>
#include <utility>
#include <vector>

namespace saida {

GpuBudget::GpuBudget(MeshCache& meshes, TextureCache& textures,
                     GpuGraveyard& graveyard)
    : meshes_(meshes), textures_(textures), graveyard_(graveyard) {}

uint64_t GpuBudget::residentBytes() const {
    return meshes_.residentBytes() + textures_.residentBytes();
}

void GpuBudget::setLiveUsage(std::unordered_set<const Mesh*> meshes,
                             std::unordered_set<AssetID> textures) {
    liveMeshes_ = std::move(meshes);
    liveTextures_ = std::move(textures);
    hasLiveUsage_ = true;
}

GpuBudget::TrimResult GpuBudget::trimUnused(
    const std::unordered_set<const Mesh*>& liveMeshes,
    const std::unordered_set<AssetID>& liveTextures) {
    const size_t texturesBefore = textures_.size();
    const size_t meshesBefore = meshes_.size();
    textures_.sweepUnused(liveTextures, graveyard_, frameClock_);
    meshes_.sweepUnused(liveMeshes, graveyard_, frameClock_);
    return {
        texturesBefore - textures_.size(),
        meshesBefore - meshes_.size(),
    };
}

void GpuBudget::enforce() {
    if (limitBytes_ == 0 || residentBytes() <= limitBytes_) {
        overBudgetWarned_ = false;
        return;
    }
    if (!hasLiveUsage_) return;

    enum class CacheTag {
        Texture,
        Mesh,
    };
    struct Candidate {
        AssetID id;
        uint64_t lastUse;
        CacheTag cache;
    };

    std::vector<Candidate> candidates;
    std::vector<TextureCache::EvictionCandidate> textureCandidates;
    textures_.collectEvictionCandidates(liveTextures_, textureCandidates);
    for (const TextureCache::EvictionCandidate& candidate : textureCandidates)
        candidates.push_back({candidate.id, candidate.lastUse, CacheTag::Texture});

    std::vector<MeshCache::EvictionCandidate> meshCandidates;
    meshes_.collectEvictionCandidates(liveMeshes_, meshCandidates);
    for (const MeshCache::EvictionCandidate& candidate : meshCandidates)
        candidates.push_back({candidate.id, candidate.lastUse, CacheTag::Mesh});

    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& a, const Candidate& b) {
                  return a.lastUse < b.lastUse;
              });

    for (const Candidate& candidate : candidates) {
        if (residentBytes() <= limitBytes_) break;
        const bool isMesh = candidate.cache == CacheTag::Mesh;
        const uint64_t bytes = isMesh
            ? meshes_.evict(candidate.id, graveyard_, frameClock_)
            : textures_.evict(candidate.id, graveyard_, frameClock_);
        if (bytes == 0) continue;

        evictedBytes_ += bytes;
        ++evictedCount_;
        Log::info("assets: gpu budget evicted ",
                  isMesh ? "mesh" : "texture", " id=", candidate.id,
                  " (", bytes, " bytes, resident ", residentBytes(), "/",
                  limitBytes_, ")");
    }

    if (residentBytes() > limitBytes_ && !overBudgetWarned_) {
        overBudgetWarned_ = true;
        Log::warn("assets: gpu budget exceeded by live content (resident ",
                  residentBytes(), " > budget ", limitBytes_,
                  "), nothing evictable");
    }
}

} // namespace saida
