#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <filesystem>

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

struct AssetMetadata {
    AssetID id = kAssetInvalid;
    std::string relativePath;
    uint64_t contentHash = 0; // Renamed from fileHash, now a true content hash
    AssetType type = AssetType::Unknown;
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
