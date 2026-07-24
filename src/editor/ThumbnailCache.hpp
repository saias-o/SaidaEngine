#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <imgui.h>

namespace saida {

class VulkanDevice;
class Texture;

// Bounded, downscaled thumbnail cache for the asset browser.
//
// Generates small (<= kThumbSize px) GPU thumbnails for image files on demand,
// keyed by absolute path. It hard-caps both the number of resident thumbnails
// (LRU eviction) and the number generated per frame, so browsing a folder full
// of large images can never blow up VRAM or stall the UI. All GPU resources are
// owned by the cache (RAII); an evicted texture is retired through a small
// deferred queue so a descriptor still in flight on the GPU is never freed under
// it. Thumbnails are independent of the asset ResourceManager — viewing images
// no longer interns full-resolution textures that live forever.
class ThumbnailCache {
public:
    ThumbnailCache() = default;
    ~ThumbnailCache();
    ThumbnailCache(const ThumbnailCache&) = delete;
    ThumbnailCache& operator=(const ThumbnailCache&) = delete;

    // Call once per UI frame before any get(): advances the frame clock and
    // releases thumbnails retired long enough ago to be GPU-safe.
    void beginFrame();

    // Returns an ImGui texture id for the file's thumbnail, or 0 if it is not
    // ready yet (the caller should draw a placeholder). Generation is bounded
    // per frame, so a not-ready result simply becomes ready on a later frame.
    ImTextureID get(VulkanDevice& device, const std::string& path);

    void clear();

private:
    struct Entry {
        std::unique_ptr<Texture> texture;
        ImTextureID imguiId = 0;
        uint64_t lastUsedFrame = 0;
        std::filesystem::file_time_type mtime{};
    };
    struct Retired {
        std::unique_ptr<Texture> texture;
        ImTextureID imguiId = 0;
        uint64_t frame = 0;
    };

    bool generate(VulkanDevice& device, const std::string& path,
                  std::filesystem::file_time_type mtime, Entry& out);
    void evictLruIfNeeded();
    void retire(Entry& entry);

    static constexpr size_t   kMaxEntries     = 256;  // resident thumbnails
    static constexpr int      kMaxNewPerFrame = 4;    // generations per frame
    static constexpr int      kThumbSize      = 128;  // max thumbnail dimension
    static constexpr uint64_t kRetireFrames   = 4;    // > frames-in-flight

    std::unordered_map<std::string, Entry> entries_;
    std::vector<Retired> retired_;
    uint64_t frame_ = 0;
    int generatedThisFrame_ = 0;
};

} // namespace saida
