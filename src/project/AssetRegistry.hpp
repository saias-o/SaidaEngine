#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <filesystem>

#include "rhi/Sampler.hpp"

namespace saida {

using AssetID = uint64_t;

constexpr AssetID kAssetInvalid = 0;
// Reserved IDs for builtin meshes
constexpr AssetID kAssetBuiltinCube = 1;

enum class AssetType {
    Unknown,
    Mesh,
    Texture,
    Material,
    Scene,
    Audio,
    Rig,
    Animation,
    Effect
};

// Project-side import settings for an asset (schema 2).
//
// Every field is optional, and absence is meaningful: it means "keep whatever
// the source asset declares" (a glTF sampler's wrap mode, for instance). A
// value here is an explicit project override, which is the point -- a texture
// exported with the wrong wrap mode can be corrected without re-exporting it,
// and without rebuilding the engine.
struct AssetImportSettings {
    std::optional<rhi::AddressMode> wrap;
    std::optional<bool> srgb;

    bool empty() const { return !wrap.has_value() && !srgb.has_value(); }
};

struct AssetMetadata {
    AssetID id = kAssetInvalid;
    std::string relativePath;
    uint64_t contentHash = 0; // Renamed from fileHash, now a true content hash
    AssetType type = AssetType::Unknown;
    AssetImportSettings import;
};

// Used internally to avoid re-hashing files that haven't been modified locally
struct LocalCacheEntry {
    uint64_t lastWriteTime = 0;
    uint64_t fileSize = 0;
    uint64_t contentHash = 0;
};

class AssetRegistry {
public:
    AssetRegistry() = default;

    // Load from asset_registry.json
    bool load(const std::string& projectRoot);
    // Save to asset_registry.json
    bool save(const std::string& projectRoot) const;

    // Scan the assets/ directory to detect new, missing, or moved files.
    void sync(const std::string& projectRoot);

    AssetID getID(const std::string& relativePath) const;
    std::string getPath(AssetID id) const;
    std::string getAbsolutePath(AssetID id) const;
    AssetType getType(AssetID id) const;

    // Project-side import overrides for an asset, empty when the project has
    // nothing to say and the source asset's own declaration should stand.
    AssetImportSettings getImportSettings(AssetID id) const;

    // Canonical form of an asset key: path made project-relative when it's
    // absolute under the root, '/' separators, sub-asset suffix ("#clip")
    // preserved. Applied by registerAsset and getID — stored keys stay
    // portable across machines.
    std::string normalizeKey(const std::string& key) const;

    // Registers a new asset or returns existing ID
    AssetID registerAsset(const std::string& relativePath, AssetType type);

    // Get all tracked assets
    const std::unordered_map<AssetID, AssetMetadata>& getAssets() const { return assetsByID_; }

private:
    AssetID generateID();
    AssetType determineType(const std::filesystem::path& path) const;
    
    // Generates a true content hash based on file chunks
    uint64_t computeTrueHash(const std::filesystem::path& path) const;
    
    void loadLocalCache(const std::string& projectRoot);
    void saveLocalCache(const std::string& projectRoot) const;

    std::string projectRoot_;
    std::unordered_map<AssetID, AssetMetadata> assetsByID_;
    std::unordered_map<std::string, AssetID> assetsByPath_;
    
    // Internal cache for hashing: maps relative path -> {timestamp, size, hash}
    std::unordered_map<std::string, LocalCacheEntry> localCache_;
};

} // namespace saida
