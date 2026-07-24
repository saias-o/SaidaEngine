#pragma once

#include "project/AssetLoader.hpp"
#include "project/AssetRegistry.hpp"

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

namespace saida {

// Cache for one asset type loaded asynchronously through the AssetLoader. It owns
// the resident objects, the in-flight handles and the last error per id, and
// collapses the identical load/get/state/error/finalize logic that RigAsset,
// ClipView and AnimGraphAsset each used to duplicate in ResourceManager.
//
// Invariant: an id is in at most one of {resident, pending}. finalize() drains a
// finished handle into resident (on success, via the extractor) or failed (on
// error) and drops it from pending. get() is null until the id is resident.
//
// Generic on purpose: it knows nothing about the payload's concrete type or its
// diagnostics — the extractor, supplied by the owner, casts the payload and does
// any logging, so this template can serve any future async asset.
template <typename T>
class AsyncAssetCache {
public:
    // Turns a Ready handle's decoded payload into the resident object, or returns
    // null to skip it (e.g. an unexpected/empty payload). `path` is the asset's
    // registry path, for the extractor's own diagnostics.
    using Extractor =
        std::function<std::unique_ptr<T>(const std::shared_ptr<void>& payload,
                                         const std::string& path)>;

    AsyncAssetCache(AssetType type, AssetPayloadKind kind,
                    std::function<AssetDecoder()> makeDecoder, Extractor extract)
        : type_(type), kind_(kind), makeDecoder_(std::move(makeDecoder)),
          extract_(std::move(extract)) {}

    // Registers the path and starts an async request; returns the stable id. A
    // no-op if already resident or pending. Clears any prior recorded failure.
    AssetID load(AssetRegistry* registry, AssetLoader& loader,
                 const std::string& path) {
        if (!registry || path.empty()) return kAssetInvalid;
        const AssetID id = registry->registerAsset(path, type_);
        if (id == kAssetInvalid || resident_.count(id) || pending_.count(id))
            return id;
        failed_.erase(id);
        AssetHandle handle =
            loader.request(id, AssetLoadPriority::High, kind_, makeDecoder_());
        if (!handle) return kAssetInvalid;
        pending_.emplace(id, std::move(handle));
        return id;
    }

    const T* get(AssetID id) const {
        auto it = resident_.find(id);
        return it != resident_.end() ? it->second.get() : nullptr;
    }

    AssetLoadState loadState(AssetID id) const {
        if (resident_.count(id)) return AssetLoadState::Ready;
        if (auto it = pending_.find(id); it != pending_.end())
            return it->second.state();
        return AssetLoadState::Failed;
    }

    std::string loadError(AssetID id) const {
        if (auto it = failed_.find(id); it != failed_.end()) return it->second;
        if (auto it = pending_.find(id); it != pending_.end())
            return it->second.error();
        return {};
    }

    // Drains finished handles: Ready -> resident (via the extractor), otherwise
    // record the error. Still-loading handles are left untouched.
    void finalize(AssetRegistry* registry) {
        for (auto it = pending_.begin(); it != pending_.end();) {
            const AssetLoadState state = it->second.state();
            if (state == AssetLoadState::Queued ||
                state == AssetLoadState::Loading) {
                ++it;
                continue;
            }
            const AssetID id = it->first;
            if (state == AssetLoadState::Ready) {
                const std::string path =
                    registry ? registry->getPath(id) : std::string();
                if (auto value = extract_(it->second.payload(), path))
                    resident_[id] = std::move(value);
            } else {
                failed_[id] = it->second.error();
            }
            it = pending_.erase(it);
        }
    }

    // Resident count — the entries a full sweep evicts.
    size_t size() const { return resident_.size(); }

    // Drops everything: resident objects, in-flight handles and recorded errors.
    void clear() {
        resident_.clear();
        pending_.clear();
        failed_.clear();
    }

private:
    AssetType type_;
    AssetPayloadKind kind_;
    std::function<AssetDecoder()> makeDecoder_;
    Extractor extract_;
    std::unordered_map<AssetID, std::unique_ptr<T>> resident_;
    std::unordered_map<AssetID, AssetHandle> pending_;
    std::unordered_map<AssetID, std::string> failed_;
};

} // namespace saida
