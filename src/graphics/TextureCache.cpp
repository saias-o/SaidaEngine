#include "graphics/TextureCache.hpp"

#include "core/Log.hpp"
#include "core/Profiler.hpp"
#include "graphics/BindlessTables.hpp"
#include "graphics/GpuGraveyard.hpp"
#include "graphics/Texture.hpp"
#ifndef SAIDA_RHI_WEBGPU
#include "graphics/VulkanDevice.hpp"
#include "graphics/Buffer.hpp"
#endif

#include <stb_image.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <string>
#include <utility>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <chrono>

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

#ifndef SAIDA_RHI_WEBGPU
struct TextureCache::PreparedTexture {
    AssetID id;
    uint64_t bytes;
    std::unique_ptr<Texture> texture;
    std::shared_ptr<Buffer> staging;
    std::string error;
};
struct TextureCache::PreparationQueue {
    struct Job { AssetID id; uint64_t bytes; PendingTexture input; };
    rhi::Device& device;
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<Job> jobs;
    std::deque<std::unique_ptr<PreparedTexture>> ready;
    bool stopping = false;
    std::thread worker;
    explicit PreparationQueue(rhi::Device& owner) : device(owner), worker([this] {
        Profiler::instance().setThreadName("Texture preparation");
        for (;;) {
            Job job;
            { std::unique_lock<std::mutex> lock(mutex);
              wake.wait(lock, [&] { return stopping || !jobs.empty(); });
              if (stopping) return;
              job = std::move(jobs.front()); jobs.pop_front(); }
            auto result = std::make_unique<PreparedTexture>();
            result->id = job.id; result->bytes = job.bytes;
            try {
                SAIDA_PROFILE_SCOPE("Resource/PrepareTextureWorker");
                const auto image = std::static_pointer_cast<DecodedImage>(job.input.handle.payload());
                const auto format = image->hdr ? rhi::Format::RGBA32Float :
                    (job.input.srgb ? rhi::Format::RGBA8Srgb : rhi::Format::RGBA8Unorm);
                result->texture = std::make_unique<Texture>(device, image->width, image->height,
                    format, true, job.input.address);
                result->staging = std::make_shared<Buffer>(device, job.bytes, rhi::BufferUsage::TransferSrc, MemoryUsage::HostVisible);
                result->staging->write(image->pixels, job.bytes);
            } catch (const std::exception& e) { result->error = e.what(); }
            { std::lock_guard<std::mutex> lock(mutex); ready.push_back(std::move(result)); }
        }
    }) {}
    ~PreparationQueue() {
        { std::lock_guard<std::mutex> lock(mutex); stopping = true; }
        wake.notify_one(); worker.join();
    }
    void submit(Job job) {
        { std::lock_guard<std::mutex> lock(mutex); jobs.push_back(std::move(job)); }
        wake.notify_one();
    }
    std::unique_ptr<PreparedTexture> take() {
        std::lock_guard<std::mutex> lock(mutex);
        if (ready.empty()) return {};
        auto result = std::move(ready.front()); ready.pop_front(); return result;
    }
};

void TextureCache::pumpPrepared(std::vector<AssetID>& completed) {
    SAIDA_PROFILE_SCOPE("Resource/FinalizeAsyncTexture");
    if (preparation_)
        while (auto ready = preparation_->take()) {
            preparing_.erase(ready->id);
            if (!ready->error.empty()) {
                Log::error("TextureCache: asset ", ready->id, " rejected (staging bytes=", ready->bytes, "): ", ready->error);
                preparationBytes_ -= ready->bytes;
                failed_.insert(ready->id); completed.push_back(ready->id);
            } else {
                residentBytes_ += ready->texture->gpuBytes();
                transferring_.push_back(std::move(ready));
            }
        }
    // FIFO row slices leave the full mip chain unpublished until the last
    // transfer completes. The staging data is immutable and fence-retained.
    while (!transferring_.empty() && transferring_.front()->texture->uploadReady()) {
        auto& front = *transferring_.front();
        auto& texture = *front.texture;
        registerBindless(&texture);
        textures_.emplace(front.id, std::move(front.texture));
        completed.push_back(front.id);
        preparationBytes_ -= front.bytes; transferring_.pop_front();
    }
    if (transferring_.empty()) return;
    auto& front = *transferring_.front();
    auto& texture = *front.texture;
    if (texture.uploadedRows() == texture.height() || device_.pendingUploads() >= rhi::Device::kMaxPendingUploads) return;
    constexpr uint64_t kUploadBytesPerFrame = 2 * 1024 * 1024;
    const uint64_t rowBytes = front.bytes / texture.height();
    const uint32_t rows = static_cast<uint32_t>(std::min<uint64_t>(texture.height()-texture.uploadedRows(),
        std::max<uint64_t>(1, kUploadBytesPerFrame/rowBytes)));
    SAIDA_PROFILE_COUNTER_ADD("Assets/TextureUploadBytes", double(rows*rowBytes));
    try { texture.uploadRows(front.staging, rows); }
    catch (const std::exception& e) {
        Log::error("TextureCache: upload ", front.id, " rejected: ", e.what());
        failed_.insert(front.id); completed.push_back(front.id);
        residentBytes_ -= texture.gpuBytes(); preparationBytes_ -= front.bytes;
        transferring_.pop_front();
    }
}
#endif

TextureCache::TextureCache(rhi::Device& device, BindlessTables& bindlessTables)
    : device_(device), bindlessTables_(bindlessTables) {
    ensureDefaultTextures();
}

TextureCache::~TextureCache() = default;

bool TextureCache::hasPendingLoads() const {
    return !pending_.empty() || !uploading_.empty()
#ifndef SAIDA_RHI_WEBGPU
        || !preparing_.empty() || !transferring_.empty()
#endif
        ;
}

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

    const bool preparing =
#ifndef SAIDA_RHI_WEBGPU
        preparing_.count(id) || std::any_of(transferring_.begin(), transferring_.end(),
            [id](const auto& item) { return item->id == id; });
#else
        false;
#endif
    if (!pending_.count(id) && !uploading_.count(id) && !preparing) {
        AssetHandle handle = loader.request(id, AssetLoadPriority::High,
                                            AssetPayloadKind::Image, makeImageDecoder());
        if (!handle) return nullptr;
        pending_.emplace(id, PendingTexture{srgb, addressModeFor(id), std::move(handle)});
    }
    return nullptr;
}

void TextureCache::finalizePending(std::vector<AssetID>& completed) {
#ifndef SAIDA_RHI_WEBGPU
    pumpPrepared(completed);
#endif
    for (auto it = uploading_.begin(); it != uploading_.end();) {
        if (!it->second->uploadReady()) { ++it; continue; }
        registerBindless(it->second.get());
        textures_.emplace(it->first, std::move(it->second));
        completed.push_back(it->first);
        it = uploading_.erase(it);
    }
    if (device_.pendingUploads() >= rhi::Device::kMaxPendingUploads) return;
#ifndef SAIDA_RHI_WEBGPU
    constexpr size_t kMaxPreparedTextures = 2;
    constexpr uint64_t kPreparationBudgetBytes = 64 * 1024 * 1024;
    if (preparing_.size() + transferring_.size() >= kMaxPreparedTextures) return;
#endif
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
#ifndef SAIDA_RHI_WEBGPU
                const uint64_t bytes = uint64_t(image->width)*image->height*(image->hdr ? 16 : 4);
                // An oversized image runs alone instead of deadlocking at the
                // soft preparation limit or silently losing source resolution.
                if (preparationBytes_ && (bytes > kPreparationBudgetBytes || preparationBytes_ > kPreparationBudgetBytes-bytes)) { ++it; continue; }
                if (!preparation_) preparation_ = std::make_unique<PreparationQueue>(device_);
                preparation_->submit({id, bytes, std::move(it->second)});
                preparing_.insert(id); preparationBytes_ += bytes;
#else
                SAIDA_PROFILE_SCOPE("Resource/FinalizeAsyncTexture");
                rhi::Format format = it->second.srgb
                    ? rhi::Format::RGBA8Srgb
                    : rhi::Format::RGBA8Unorm;
                auto texture = std::make_unique<Texture>(
                    device_, static_cast<const uint8_t*>(image->pixels),
                    image->width, image->height, format, true, it->second.address, true);
                residentBytes_ += texture->gpuBytes();
                uploading_.emplace(id, std::move(texture));
#endif
                created = true;
            }
        }
        if (!created) failed_.insert(id);
        it = pending_.erase(it);
        if (!created) completed.push_back(id);
        // Enqueue at most one preparation (one image on Web) per pump.
        if (created) break;
    }
}
AssetID TextureCache::queueMemory(const uint8_t* data, size_t size, bool srgb,
                                  rhi::AddressMode address, AssetLoader& loader) {
    static std::atomic<AssetID> dynamicId{0x8200000000000000ULL};
    if (!data || !size) return kAssetInvalid;
    const AssetID id = dynamicId++;
    auto handle = loader.requestMemory(id, std::vector<uint8_t>(data, data + size),
                                      AssetPayloadKind::Image, makeImageDecoder());
    pending_.emplace(id, PendingTexture{srgb, address, std::move(handle)});
    return id;
}

void TextureCache::setAddressMode(AssetID id, rhi::AddressMode address) {
    if (id == kAssetInvalid) return;
    // Repeat is the default: recording it would grow the map for nothing.
    if (address == rhi::AddressMode::Repeat) addressModes_.erase(id);
    else addressModes_[id] = address;
}

rhi::AddressMode TextureCache::addressModeFor(AssetID id) const {
    auto it = addressModes_.find(id);
    return it == addressModes_.end() ? rhi::AddressMode::Repeat : it->second;
}

AssetID TextureCache::registerMemory(const uint8_t* data, size_t size, bool srgb,
                                     rhi::AddressMode address) {
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
        static_cast<uint32_t>(height), format, true, address);
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
        // Normal vectors are linear data: decoding 128 as sRGB turns a neutral
        // tangent-plane component into -0.568 and tilts every untextured normal.
        const uint8_t normal[] = {128, 128, 255, 255};
        defaultNormal_ = std::make_unique<Texture>(
            device_, normal, 1, 1, rhi::Format::RGBA8Unorm);
        registerBindless(defaultNormal_.get());
    }
    missing();
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
#ifndef SAIDA_RHI_WEBGPU
    preparation_.reset(); preparing_.clear(); transferring_.clear(); preparationBytes_ = 0;
#endif
    uploading_.clear();
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
