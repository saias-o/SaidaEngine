#include "editor/ThumbnailCache.hpp"

#include "graphics/Texture.hpp"

#include <backends/imgui_impl_vulkan.h>

#include <algorithm>
#include <system_error>

#include "stb_image.h"

namespace saida {
namespace fs = std::filesystem;

ThumbnailCache::~ThumbnailCache() { clear(); }

void ThumbnailCache::beginFrame() {
    ++frame_;
    generatedThisFrame_ = 0;
    // Release thumbnails retired long enough ago that no in-flight frame can
    // still reference their descriptor or image.
    for (auto it = retired_.begin(); it != retired_.end();) {
        if (frame_ - it->frame >= kRetireFrames) {
            if (it->imguiId) ImGui_ImplVulkan_RemoveTexture((VkDescriptorSet)it->imguiId);
            it = retired_.erase(it);  // unique_ptr<Texture> frees the GPU image
        } else {
            ++it;
        }
    }
}

ImTextureID ThumbnailCache::get(VulkanDevice& device, const std::string& path) {
    std::error_code ec;
    const fs::file_time_type mtime = fs::last_write_time(path, ec);
    if (ec) return 0;

    if (auto it = entries_.find(path); it != entries_.end()) {
        if (it->second.mtime == mtime) {  // fresh cache hit
            it->second.lastUsedFrame = frame_;
            return it->second.imguiId;
        }
        retire(it->second);               // file changed on disk → regenerate
        entries_.erase(it);
    }

    if (generatedThisFrame_ >= kMaxNewPerFrame) return 0;  // budget spent this frame

    Entry entry;
    if (!generate(device, path, mtime, entry)) return 0;
    ++generatedThisFrame_;

    evictLruIfNeeded();
    const ImTextureID id = entry.imguiId;
    entries_.emplace(path, std::move(entry));
    return id;
}

bool ThumbnailCache::generate(VulkanDevice& device, const std::string& path,
                              fs::file_time_type mtime, Entry& out) {
    int w = 0, h = 0, channels = 0;
    stbi_uc* pixels = stbi_load(path.c_str(), &w, &h, &channels, STBI_rgb_alpha);
    if (!pixels || w <= 0 || h <= 0) {
        if (pixels) stbi_image_free(pixels);
        return false;
    }

    // Target preserves aspect ratio, longest edge == kThumbSize (never upscale).
    int tw = w, th = h;
    if (w > kThumbSize || h > kThumbSize) {
        if (w >= h) { tw = kThumbSize; th = std::max(1, h * kThumbSize / w); }
        else        { th = kThumbSize; tw = std::max(1, w * kThumbSize / h); }
    }

    std::vector<uint8_t> thumb(static_cast<size_t>(tw) * th * 4);
    // Area-average (box) downscale: each destination texel is the mean of its
    // source footprint — cheap, allocation-free, and free of the worst aliasing.
    for (int y = 0; y < th; ++y) {
        const int sy0 = y * h / th;
        const int sy1 = std::max(sy0 + 1, (y + 1) * h / th);
        for (int x = 0; x < tw; ++x) {
            const int sx0 = x * w / tw;
            const int sx1 = std::max(sx0 + 1, (x + 1) * w / tw);
            uint32_t r = 0, g = 0, b = 0, a = 0, n = 0;
            for (int sy = sy0; sy < sy1; ++sy) {
                for (int sx = sx0; sx < sx1; ++sx) {
                    const stbi_uc* p = pixels + (static_cast<size_t>(sy) * w + sx) * 4;
                    r += p[0]; g += p[1]; b += p[2]; a += p[3]; ++n;
                }
            }
            uint8_t* d = thumb.data() + (static_cast<size_t>(y) * tw + x) * 4;
            d[0] = static_cast<uint8_t>(r / n);
            d[1] = static_cast<uint8_t>(g / n);
            d[2] = static_cast<uint8_t>(b / n);
            d[3] = static_cast<uint8_t>(a / n);
        }
    }
    stbi_image_free(pixels);

    out.texture = std::make_unique<Texture>(
        device, thumb.data(), static_cast<uint32_t>(tw), static_cast<uint32_t>(th),
        rhi::Format::RGBA8Srgb, /*generateMipmaps=*/true);
    out.imguiId = (ImTextureID)ImGui_ImplVulkan_AddTexture(
        out.texture->sampler(), out.texture->imageView(),
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    out.lastUsedFrame = frame_;
    out.mtime = mtime;
    return true;
}

void ThumbnailCache::evictLruIfNeeded() {
    while (entries_.size() >= kMaxEntries) {
        auto lru = entries_.begin();
        for (auto it = entries_.begin(); it != entries_.end(); ++it)
            if (it->second.lastUsedFrame < lru->second.lastUsedFrame) lru = it;
        retire(lru->second);
        entries_.erase(lru);
    }
}

void ThumbnailCache::retire(Entry& entry) {
    if (!entry.texture && !entry.imguiId) return;
    retired_.push_back({std::move(entry.texture), entry.imguiId, frame_});
    entry.imguiId = 0;
}

void ThumbnailCache::clear() {
    for (auto& [path, e] : entries_)
        if (e.imguiId) ImGui_ImplVulkan_RemoveTexture((VkDescriptorSet)e.imguiId);
    entries_.clear();
    for (auto& r : retired_)
        if (r.imguiId) ImGui_ImplVulkan_RemoveTexture((VkDescriptorSet)r.imguiId);
    retired_.clear();
}

} // namespace saida
