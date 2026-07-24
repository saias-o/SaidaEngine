#pragma once

// AnimationCache — on-disk cache of cooked clips, indexed by content hash
// (sources + settings + cooker/format versions). .sanimc files are derived
// and regenerable: they are never a source of truth. Writes are atomic
// (temp file then rename) to survive interruptions.

#include "scene/animation/AnimationCooker.hpp"

#include <memory>
#include <string>

namespace saida {

class AnimationCache {
public:
    // `cacheDir` is created on demand (e.g. <project>/.saida/cache/animation).
    explicit AnimationCache(std::string cacheDir);

    struct Result {
        std::shared_ptr<const CookedClip> clip;
        bool fromCache = false;
        CookReport report;  // filled in only when the clip was just cooked
    };

    Result getOrCook(const AnimationClip& clip, const Rig& rig,
                     const CookSettings& settings = {},
                     const RetargetMap* retarget = nullptr);

    std::string cachePath(uint64_t contentHash) const;

private:
    std::string cacheDir_;
};

} // namespace saida
