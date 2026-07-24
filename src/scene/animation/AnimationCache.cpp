#include "scene/animation/AnimationCache.hpp"

#include "core/Log.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <system_error>

namespace saida {

namespace fs = std::filesystem;

AnimationCache::AnimationCache(std::string cacheDir) : cacheDir_(std::move(cacheDir)) {}

std::string AnimationCache::cachePath(uint64_t contentHash) const {
    char name[32];
    std::snprintf(name, sizeof(name), "%016llx.sanimc",
                  static_cast<unsigned long long>(contentHash));
    return (fs::path(cacheDir_) / name).string();
}

AnimationCache::Result AnimationCache::getOrCook(const AnimationClip& clip, const Rig& rig,
                                                 const CookSettings& settings,
                                                 const RetargetMap* retarget) {
    Result result;
    const uint64_t hash = AnimationCooker::contentHash(clip, rig, settings, retarget);
    const fs::path path = cachePath(hash);

    std::error_code ec;
    if (fs::exists(path, ec)) {
        std::ifstream in(path, std::ios::binary);
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                   std::istreambuf_iterator<char>());
        auto cooked = std::make_shared<CookedClip>();
        std::string error;
        if (in && CookedClip::deserialize(bytes.data(), bytes.size(), *cooked, &error)) {
            result.clip = std::move(cooked);
            result.fromCache = true;
            return result;
        }
        Log::warn("animation cache: rejecting '", path.string(), "' (", error,
                  "), recooking");
    }

    auto cooked = std::make_shared<CookedClip>(
        AnimationCooker::cook(clip, rig, settings, &result.report, retarget));

    fs::create_directories(cacheDir_, ec);
    const fs::path temp = path.string() + ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        const std::vector<uint8_t> bytes = cooked->serialize();
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  std::streamsize(bytes.size()));
        if (!out) {
            Log::warn("animation cache: failed to write '", temp.string(), "'");
            fs::remove(temp, ec);
            result.clip = std::move(cooked);
            return result;
        }
    }
    fs::rename(temp, path, ec);
    if (ec) {
        Log::warn("animation cache: failed to publish '", path.string(),
                  "': ", ec.message());
        fs::remove(temp, ec);
    }

    result.clip = std::move(cooked);
    return result;
}

} // namespace saida
