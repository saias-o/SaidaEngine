#include "graphics/TextureCache.hpp"

#include "core/Log.hpp"
#include "core/Profiler.hpp"
#include "graphics/BindlessTables.hpp"
#include "graphics/GpuGraveyard.hpp"
#include "graphics/Texture.hpp"

#include <stb_image.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <string>
#include <utility>

namespace saida {

namespace {

// Decoded off the main thread; upload and bindless registration happen during
// TextureCache::finalizePending on the main thread.
struct DecodedImage {
    void* pixels = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    bool hdr = false;

    ~DecodedImage() {
        if (pixels) stbi_image_free(pixels);
    }
};

AssetDecoder makeImageDecoder() {
    return [](std::vector<uint8_t>&& bytes, AssetDecodeResult& out, std::string& error) {
        auto image = std::make_shared<DecodedImage>();
        int width = 0;
        int height = 0;
        int channels = 0;
        const auto* data = bytes.data();
        const int size = static_cast<int>(bytes.size());
#ifndef SAIDA_RHI_WEBGPU
        image->hdr = size > 0 && stbi_is_hdr_from_memory(data, size) != 0;
        if (image->hdr)
            image->pixels = stbi_loadf_from_memory(
                data, size, &width, &height, &channels, STBI_rgb_alpha);
        else
#endif
            image->pixels = stbi_load_from_memory(
                data, size, &width, &height, &channels, STBI_rgb_alpha);
        if (!image->pixels) {
            const char* reason = stbi_failure_reason();
            error = reason ? reason : "unrecognized image";
            return false;
        }
        image->width = static_cast<uint32_t>(width);
        image->height = static_cast<uint32_t>(height);
        out.payload = image;
        out.bytes = static_cast<uint64_t>(image->width) * image->height *
                    (image->hdr ? 16 : 4);
        return true;
    };
}

} // namespace

TextureCache::TextureCache(rhi::Device& device, BindlessTables& bindlessTables)
    : device_(device), bindlessTables_(bindlessTables) {
    ensureDefaultTextures();
}

TextureCache::~TextureCache() = default;

void TextureCache::registerBindless(Texture* texture) {
    bindlessTables_.ensureTextureIndex(texture);
}

Texture* TextureCache::get(AssetID id, bool srgb, AssetRegistry* registry,
                           AssetLoader& loader, uint64_t frameClock) {
    if (id == kAssetInvalid) return nullptr;
    if (auto it = textures_.find(id); it != textures_.end()) {
        lastUse_[id] = frameClock;
        return it->second.get();
    }
    if (failed_.count(id)) return missing();
    if (!registry) return nullptr;

    if (!pending_.count(id)) {
        AssetHandle handle = loader.request(id, AssetLoadPriority::High,
                                            AssetPayloadKind::Image, makeImageDecoder());
        if (!handle) return nullptr;
        pending_.emplace(id, PendingTexture{srgb, std::move(handle)});
    }
    return nullptr;
}

void TextureCache::finalizePending(std::vector<AssetID>& completed) {
    for (auto it = pending_.begin(); it != pending_.end();) {
        const AssetLoadState state = it->second.handle.state();
        if (state == AssetLoadState::Queued || state == AssetLoadState::Loading) {
            ++it;
            continue;
        }

        const AssetID id = it->first;
        bool created = false;
        if (state == AssetLoadState::Ready) {
            if (auto image =
                    std::static_pointer_cast<DecodedImage>(it->second.handle.payload())) {
                SAIDA_PROFILE_SCOPE("Resource/FinalizeAsyncTexture");
                rhi::Format format = it->second.srgb
                    ? rhi::Format::RGBA8Srgb
                    : rhi::Format::RGBA8Unorm;
#ifndef SAIDA_RHI_WEBGPU
                if (image->hdr) format = rhi::Format::RGBA32Float;
#endif
                auto texture = std::make_unique<Texture>(
                    device_, static_cast<const uint8_t*>(image->pixels),
                    image->width, image->height, format);
                registerBindless(texture.get());
                residentBytes_ += texture->gpuBytes();
                textures_.emplace(id, std::move(texture));
                created = true;
            }
        }
        if (!created) failed_.insert(id);
        it = pending_.erase(it);
        completed.push_back(id);
    }
}

AssetID TextureCache::registerMemory(const uint8_t* data, size_t size, bool srgb) {
    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_uc* pixels = stbi_load_from_memory(
        data, static_cast<int>(size), &width, &height, &channels, STBI_rgb_alpha);
    if (!pixels) {
        Log::error("Failed to load memory texture");
        return kAssetInvalid;
    }

    const rhi::Format format =
        srgb ? rhi::Format::RGBA8Srgb : rhi::Format::RGBA8Unorm;
    auto texture = std::make_unique<Texture>(
        device_, pixels, static_cast<uint32_t>(width),
        static_cast<uint32_t>(height), format);
    registerBindless(texture.get());
    stbi_image_free(pixels);
    residentBytes_ += texture->gpuBytes();

    static std::atomic<AssetID> s_dynamicId{0x8000000000000000ULL};
    const AssetID id = s_dynamicId++;
    textures_.emplace(id, std::move(texture));
    return id;
}

AssetID TextureCache::registerGenerated(const uint8_t* pixels, uint32_t width,
                                        uint32_t height, rhi::Format format,
                                        bool generateMipmaps) {
    if (!pixels || width == 0 || height == 0) return kAssetInvalid;

    auto texture = std::make_unique<Texture>(
        device_, pixels, width, height, format, generateMipmaps);
    registerBindless(texture.get());
    residentBytes_ += texture->gpuBytes();

    static std::atomic<AssetID> s_generatedId{0x8100000000000000ULL};
    const AssetID id = s_generatedId++;
    textures_.emplace(id, std::move(texture));
    return id;
}

void TextureCache::ensureDefaultTextures() {
    if (!defaultWhite_) {
        const uint8_t white[] = {255, 255, 255, 255};
        defaultWhite_ = std::make_unique<Texture>(
            device_, white, 1, 1, rhi::Format::RGBA8Srgb);
        registerBindless(defaultWhite_.get());
    }
    if (!defaultNormal_) {
        const uint8_t normal[] = {128, 128, 255, 255};
        defaultNormal_ = std::make_unique<Texture>(
            device_, normal, 1, 1, rhi::Format::RGBA8Srgb);
        registerBindless(defaultNormal_.get());
    }
}

Texture* TextureCache::defaultWhite() {
    ensureDefaultTextures();
    return defaultWhite_.get();
}

Texture* TextureCache::defaultNormal() {
    ensureDefaultTextures();
    return defaultNormal_.get();
}

Texture* TextureCache::missing() {
    if (!missing_) {
        const uint8_t pixels[2 * 2 * 4] = {
            255, 0, 255, 255, 24, 24, 24, 255,
            24, 24, 24, 255, 255, 0, 255, 255,
        };
        missing_ = std::make_unique<Texture>(
            device_, pixels, 2, 2, rhi::Format::RGBA8Srgb);
        registerBindless(missing_.get());
    }
    return missing_.get();
}

uint64_t TextureCache::sweepUnused(const std::unordered_set<AssetID>& live,
                                   GpuGraveyard& graveyard, uint64_t frameClock) {
    uint64_t retiredBytes = 0;
    for (auto it = textures_.begin(); it != textures_.end();) {
        if (live.count(it->first)) {
            ++it;
            continue;
        }

        const uint64_t bytes = it->second->gpuBytes();
        retiredBytes += bytes;
        residentBytes_ -= std::min(residentBytes_, bytes);
        lastUse_.erase(it->first);
        Retired retired;
        retired.bindlessIndex = it->second->bindlessIndex();
        retired.texture = std::move(it->second);
        graveyard.retire(std::move(retired), frameClock);
        it = textures_.erase(it);
    }
    return retiredBytes;
}

void TextureCache::collectEvictionCandidates(
    const std::unordered_set<AssetID>& live,
    std::vector<EvictionCandidate>& out) const {
    for (const auto& [id, texture] : textures_) {
        (void)texture;
        if (live.count(id) || pending_.count(id)) continue;
        const auto use = lastUse_.find(id);
        out.push_back({id, use != lastUse_.end() ? use->second : 0});
    }
}

uint64_t TextureCache::evict(AssetID id, GpuGraveyard& graveyard,
                             uint64_t frameClock) {
    const auto it = textures_.find(id);
    if (it == textures_.end() || pending_.count(id)) return 0;

    const uint64_t bytes = it->second->gpuBytes();
    residentBytes_ -= std::min(residentBytes_, bytes);
    lastUse_.erase(id);
    Retired retired;
    retired.bindlessIndex = it->second->bindlessIndex();
    retired.texture = std::move(it->second);
    textures_.erase(it);
    graveyard.retire(std::move(retired), frameClock);
    return bytes;
}

void TextureCache::clear() {
    pending_.clear();
    failed_.clear();
    lastUse_.clear();
    textures_.clear();
    missing_.reset();
    defaultNormal_.reset();
    defaultWhite_.reset();
    residentBytes_ = 0;
}

} // namespace saida
