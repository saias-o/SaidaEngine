#pragma once

#include "project/AssetRegistry.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace saida {

enum class AssetLoadState : uint8_t { Queued, Loading, Ready, Failed };
enum class AssetLoadPriority : uint8_t { Low, Normal, High, Critical };

// Distinguishes requests for the same AssetID by the requested form: raw
// bytes (JS assets.load) and a decoded payload (texture, mesh) coexist
// without colliding in the entry cache.
enum class AssetPayloadKind : uint8_t {
    Raw = 0,
    Image = 1,
    MeshObj = 2,
    RigAsset = 3,
    ClipView = 4,
    AnimGraph = 5
};

// Result of a decode run on the worker (desktop) or inside pump() (web): an
// opaque payload typed by the caller, plus its real size for accounting
// (replaces the file size once the raw bytes are freed).
struct AssetDecodeResult {
    std::shared_ptr<void> payload;
    uint64_t bytes = 0;
};

// Consumes the file's raw bytes and produces the decoded payload. Returns
// false + `error` on failure. Runs off the main thread on desktop: must not
// touch the GPU or engine state.
using AssetDecoder =
    std::function<bool(std::vector<uint8_t>&& bytes, AssetDecodeResult& out, std::string& error)>;

struct AssetLoadStats {
    uint32_t live = 0;
    uint32_t queued = 0;
    uint32_t loading = 0;
    uint32_t ready = 0;
    uint32_t failed = 0;
    // Running total of requests that failed since boot (never decremented,
    // even once the handle is released) — the CI "content rejected" criterion.
    uint64_t failedTotal = 0;
    // Assets served by an on-demand network fetch (streamed web package).
    uint64_t streamedFetches = 0;
    uint64_t residentBytes = 0;
    uint64_t budgetBytes = 0;
};

class AssetLoader;

class AssetHandle {
public:
    AssetHandle() = default;

    AssetID id() const;
    AssetLoadState state() const;
    bool ready() const { return state() == AssetLoadState::Ready; }
    bool failed() const { return state() == AssetLoadState::Failed; }
    const std::vector<uint8_t>& bytes() const;
    // Payload produced by the request's decoder (null for a Raw request or
    // while the asset isn't Ready yet).
    std::shared_ptr<void> payload() const;
    std::string error() const;
    uint32_t referenceCount() const;
    explicit operator bool() const { return entry_ != nullptr; }
    void reset() { entry_.reset(); }

private:
    struct Entry;
    explicit AssetHandle(std::shared_ptr<Entry> entry) : entry_(std::move(entry)) {}
    std::shared_ptr<Entry> entry_;
    friend class AssetLoader;
};

class AssetLoader {
public:
    explicit AssetLoader(AssetRegistry* registry = nullptr,
                         uint64_t budgetBytes = 256ull * 1024ull * 1024ull);
    ~AssetLoader();
    AssetLoader(const AssetLoader&) = delete;
    AssetLoader& operator=(const AssetLoader&) = delete;

    void setRegistry(AssetRegistry* registry) { registry_ = registry; }
    AssetHandle request(AssetID id,
                        AssetLoadPriority priority = AssetLoadPriority::Normal,
                        AssetPayloadKind kind = AssetPayloadKind::Raw,
                        AssetDecoder decoder = {});
    AssetHandle request(const std::string& path, AssetType type = AssetType::Unknown,
                        AssetLoadPriority priority = AssetLoadPriority::Normal,
                        AssetPayloadKind kind = AssetPayloadKind::Raw,
                        AssetDecoder decoder = {});
    std::vector<AssetHandle> preload(const std::vector<AssetID>& ids,
                                     AssetLoadPriority priority = AssetLoadPriority::High);

    void pump();
    void collectGarbage();
    AssetLoadStats stats() const;
    void setBudget(uint64_t bytes);

    struct Accounting {
        std::atomic<uint64_t> residentBytes{0};
        std::atomic<uint64_t> budgetBytes{0};
    };

    // Running total of load failures since boot (never decremented).
    uint64_t failedTotal() const { return failedTotal_.load(std::memory_order_relaxed); }

private:
    friend class AssetHandle;
    static std::atomic<uint64_t> failedTotal_;
    std::atomic<uint64_t> streamedFetches_{0};

    struct Job {
        AssetLoadPriority priority = AssetLoadPriority::Normal;
        uint64_t sequence = 0;
        std::shared_ptr<AssetHandle::Entry> entry;
    };
    struct JobOrder {
        bool operator()(const Job& a, const Job& b) const;
    };

    struct EntryKey {
        AssetID id = kAssetInvalid;
        AssetPayloadKind kind = AssetPayloadKind::Raw;
        bool operator==(const EntryKey& o) const { return id == o.id && kind == o.kind; }
    };
    struct EntryKeyHash {
        size_t operator()(const EntryKey& k) const {
            return std::hash<AssetID>()(k.id) ^ (static_cast<size_t>(k.kind) * 0x9e3779b97f4a7c15ull);
        }
    };

    AssetHandle requestResolved(AssetID id, const std::string& absolutePath,
                                AssetLoadPriority priority, AssetPayloadKind kind,
                                AssetDecoder decoder);
    bool popJob(Job& job);
    void load(const std::shared_ptr<AssetHandle::Entry>& entry);
    void finishLoad(const std::shared_ptr<AssetHandle::Entry>& entry,
                    std::vector<uint8_t>&& bytes);
#ifdef __EMSCRIPTEN__
    struct StreamCtx;
    void streamFetch(const std::shared_ptr<AssetHandle::Entry>& entry);
#endif
    void workerMain();

    AssetRegistry* registry_ = nullptr;
    std::shared_ptr<Accounting> accounting_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::priority_queue<Job, std::vector<Job>, JobOrder> jobs_;
    std::unordered_map<EntryKey, std::weak_ptr<AssetHandle::Entry>, EntryKeyHash> entries_;
    uint64_t nextSequence_ = 1;
    bool stopping_ = false;
#ifndef __EMSCRIPTEN__
    std::thread worker_;
#endif
};

const char* assetLoadStateName(AssetLoadState state);

} // namespace saida
