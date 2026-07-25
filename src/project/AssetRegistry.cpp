#include "project/AssetRegistry.hpp"
#include "core/FormatVersions.hpp"
#include "core/Log.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <random>

namespace saida {

namespace {

const char* wrapToString(rhi::AddressMode mode) {
    return mode == rhi::AddressMode::Repeat ? "repeat" : "clamp";
}

// An unreadable import block is reported and dropped, never guessed at: a
// silently ignored override would send the author hunting through Blender for a
// setting the project had already tried to fix.
AssetImportSettings readImportSettings(const nlohmann::json& entry,
                                       const std::string& path) {
    AssetImportSettings out;
    const auto it = entry.find("import");
    if (it == entry.end()) return out;
    if (!it->is_object()) {
        Log::warn("AssetRegistry: 'import' must be an object for '", path, "'");
        return out;
    }

    if (const auto wrap = it->find("wrap"); wrap != it->end()) {
        const std::string value = wrap->is_string() ? wrap->get<std::string>() : "";
        if (value == "repeat") out.wrap = rhi::AddressMode::Repeat;
        else if (value == "clamp") out.wrap = rhi::AddressMode::ClampToEdge;
        else Log::warn("AssetRegistry: unknown import wrap '", value, "' for '", path,
                       "' (expected \"repeat\" or \"clamp\")");
    }
    if (const auto srgb = it->find("srgb"); srgb != it->end()) {
        if (srgb->is_boolean()) out.srgb = srgb->get<bool>();
        else Log::warn("AssetRegistry: import srgb must be a boolean for '", path, "'");
    }
    return out;
}

} // namespace


namespace {
using json = nlohmann::json;
const char* kRegistryFilename = "asset_registry.json";
const char* kLocalCacheFilename = "asset_registry.local.json";

constexpr uint64_t FNV_PRIME = 1099511628211ULL;
constexpr uint64_t FNV_OFFSET = 14695981039346656037ULL;

uint64_t fnv1a_64(const void* data, size_t size, uint64_t hash = FNV_OFFSET) {
    const uint8_t* ptr = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; ++i) {
        hash ^= ptr[i];
        hash *= FNV_PRIME;
    }
    return hash;
}

std::string assetTypeToString(AssetType type) {
    switch (type) {
        case AssetType::Mesh: return "Mesh";
        case AssetType::Texture: return "Texture";
        case AssetType::Material: return "Material";
        case AssetType::Scene: return "Scene";
        case AssetType::Audio: return "Audio";
        case AssetType::Rig: return "Rig";
        case AssetType::Animation: return "Animation";
        case AssetType::Effect: return "Effect";
        default: return "Unknown";
    }
}

AssetType stringToAssetType(const std::string& str) {
    if (str == "Mesh") return AssetType::Mesh;
    if (str == "Texture") return AssetType::Texture;
    if (str == "Material") return AssetType::Material;
    if (str == "Scene") return AssetType::Scene;
    if (str == "Audio") return AssetType::Audio;
    if (str == "Rig") return AssetType::Rig;
    if (str == "Animation") return AssetType::Animation;
    if (str == "Effect") return AssetType::Effect;
    return AssetType::Unknown;
}
} // namespace

bool AssetRegistry::load(const std::string& projectRoot) {
    projectRoot_ = projectRoot;
    std::filesystem::path path = std::filesystem::path(projectRoot) / kRegistryFilename;
    if (!std::filesystem::exists(path)) {
        Log::info("No asset registry found, a new one will be created.");
        return true; // Not an error to have no registry initially
    }

    std::ifstream file(path);
    if (!file.is_open()) {
        Log::error("AssetRegistry: Failed to open ", path.string());
        return false;
    }

    try {
        json j;
        file >> j;
        if (!j.is_object()) {
            Log::error("AssetRegistry: registry root must be an object: ", path.string());
            return false;
        }

        if (const std::string envelope = format::schemaEnvelopeError(
                j, format::kAssetRegistryVersion, "asset registry");
            !envelope.empty()) {
            Log::error("AssetRegistry: ", envelope, ": ", path.string());
            return false;
        }

        if (!j.contains("assets")) {
            Log::error("AssetRegistry: missing 'assets': ", path.string());
            return false;
        }
        const json* assetsJson = &j["assets"];
        if (!assetsJson->is_object()) {
            Log::error("AssetRegistry: 'assets' must be an object: ", path.string());
            return false;
        }
        
        std::unordered_map<AssetID, AssetMetadata> tempAssetsByID;
        std::unordered_map<std::string, AssetID> tempAssetsByPath;

        for (const auto& [idStr, entryJson] : assetsJson->items()) {
            AssetMetadata meta;
            meta.id = std::stoull(idStr);
            meta.relativePath = entryJson.value("path", "");
            meta.contentHash = entryJson.value("hash", 0ULL);
            meta.type = stringToAssetType(entryJson.value("type", "Unknown"));
            meta.import = readImportSettings(entryJson, meta.relativePath);

            tempAssetsByID[meta.id] = meta;
            tempAssetsByPath[meta.relativePath] = meta.id;
        }
        
        assetsByID_ = std::move(tempAssetsByID);
        assetsByPath_ = std::move(tempAssetsByPath);
        
        Log::info("Loaded ", assetsByID_.size(), " assets from registry.");
    } catch (const std::exception& e) {
        Log::error("AssetRegistry: JSON parsing error: ", e.what());
        return false;
    }
    return true;
}

bool AssetRegistry::save(const std::string& projectRoot) const {
    std::filesystem::path path = std::filesystem::path(projectRoot) / kRegistryFilename;
    // Keep the checked-in registry byte-identical on every platform. Text mode
    // expands '\n' to CRLF on Windows, which makes an otherwise unchanged
    // registry dirty after an editor build.
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
        Log::error("AssetRegistry: Failed to write to ", path.string());
        return false;
    }

    json assets = json::object();
    
    // Sort by path for predictable diffs in git
    std::vector<AssetMetadata> sortedAssets;
    for (const auto& [id, meta] : assetsByID_) sortedAssets.push_back(meta);
    std::sort(sortedAssets.begin(), sortedAssets.end(), [](const auto& a, const auto& b) {
        return a.relativePath < b.relativePath;
    });

    for (const auto& meta : sortedAssets) {
        json entry = {
            {"path", meta.relativePath},
            {"hash", meta.contentHash},
            {"type", assetTypeToString(meta.type)}
        };
        // Only written when the project actually overrides something: an empty
        // block on every asset would bury the few real overrides in noise.
        if (!meta.import.empty()) {
            json imp = json::object();
            if (meta.import.wrap) imp["wrap"] = wrapToString(*meta.import.wrap);
            if (meta.import.srgb) imp["srgb"] = *meta.import.srgb;
            entry["import"] = std::move(imp);
        }
        assets[std::to_string(meta.id)] = std::move(entry);
    }

    json j = {{"assets", std::move(assets)}};
    format::writeSchema(j, format::kAssetRegistryVersion);

    file << j.dump(4);
    return true;
}

void AssetRegistry::sync(const std::string& projectRoot) {
    std::filesystem::path root(projectRoot);
    std::filesystem::path assetsDir = root / "assets";
    std::filesystem::path scenesDir = root / "scenes";

    std::unordered_map<std::string, uint64_t> currentFiles;

    loadLocalCache(projectRoot);

    auto scanDir = [&](const std::filesystem::path& dir) {
        if (!std::filesystem::exists(dir)) return;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(dir)) {
            if (entry.is_regular_file()) {
                std::string relPath = std::filesystem::relative(entry.path(), root).string();
                std::replace(relPath.begin(), relPath.end(), '\\', '/');
                
                std::error_code ec;
                uint64_t lastWrite = static_cast<uint64_t>(std::filesystem::last_write_time(entry.path(), ec).time_since_epoch().count());
                uint64_t size = std::filesystem::file_size(entry.path(), ec);

                uint64_t hash = 0;
                auto it = localCache_.find(relPath);
                if (it != localCache_.end() && it->second.lastWriteTime == lastWrite && it->second.fileSize == size) {
                    hash = it->second.contentHash;
                } else {
                    hash = computeTrueHash(entry.path());
                    localCache_[relPath] = {lastWrite, size, hash};
                }
                currentFiles[relPath] = hash;
            }
        }
    };

    scanDir(assetsDir);
    scanDir(scenesDir);

    std::vector<AssetID> missingAssets;
    
    for (const auto& [id, meta] : assetsByID_) {
        if (currentFiles.find(meta.relativePath) == currentFiles.end()) {
            missingAssets.push_back(id);
        } else {
            assetsByID_[id].contentHash = currentFiles[meta.relativePath];
        }
    }

    for (auto it = currentFiles.begin(); it != currentFiles.end(); ) {
        const std::string& relPath = it->first;
        uint64_t hash = it->second;

        if (assetsByPath_.find(relPath) != assetsByPath_.end()) {
            ++it;
            continue;
        }

        bool healed = false;
        AssetType type = determineType(root / relPath);

        for (auto missingIt = missingAssets.begin(); missingIt != missingAssets.end(); ++missingIt) {
            AssetID missingId = *missingIt;
            if (assetsByID_[missingId].contentHash == hash && assetsByID_[missingId].type == type) {
                // Match found! Auto-heal
                Log::info("Asset Auto-Heal: Re-mapped ", assetsByID_[missingId].relativePath, " to ", relPath);
                
                assetsByPath_.erase(assetsByID_[missingId].relativePath);
                assetsByID_[missingId].relativePath = relPath;
                assetsByPath_[relPath] = missingId;
                
                missingAssets.erase(missingIt);
                healed = true;
                break;
            }
        }

        if (!healed) {
            // Truly new file, register it
            AssetID newId = registerAsset(relPath, type);
            assetsByID_[newId].contentHash = hash;
            Log::info("AssetRegistry: Tracked new file ", relPath);
        }
        
        ++it;
    }

    // Unresolved missing files remain in the registry but are practically dead links.
    for (AssetID id : missingAssets) {
        Log::warn("Asset missing: ", assetsByID_[id].relativePath);
    }
    
    saveLocalCache(projectRoot);
}

AssetID AssetRegistry::getID(const std::string& relativePath) const {
    auto it = assetsByPath_.find(normalizeKey(relativePath));
    return it != assetsByPath_.end() ? it->second : kAssetInvalid;
}

std::string AssetRegistry::getPath(AssetID id) const {
    auto it = assetsByID_.find(id);
    return it != assetsByID_.end() ? it->second.relativePath : "";
}

std::string AssetRegistry::getAbsolutePath(AssetID id) const {
    std::string relPath = getPath(id);
    if (relPath.empty() || projectRoot_.empty()) return "";
    return (std::filesystem::path(projectRoot_) / relPath).string();
}

AssetType AssetRegistry::getType(AssetID id) const {
    auto it = assetsByID_.find(id);
    return it != assetsByID_.end() ? it->second.type : AssetType::Unknown;
}

AssetImportSettings AssetRegistry::getImportSettings(AssetID id) const {
    auto it = assetsByID_.find(id);
    return it != assetsByID_.end() ? it->second.import : AssetImportSettings{};
}

AssetID AssetRegistry::registerAsset(const std::string& relativePath, AssetType type) {
    // Stable, portable identity: call sites sometimes pass absolute paths
    // (drag-drop, MCP) — the stored key is always project-relative.
    const std::string key = normalizeKey(relativePath);
    if (auto existing = getID(key); existing != kAssetInvalid) {
        return existing; // Already registered
    }

    AssetMetadata meta;
    meta.id = generateID();
    meta.relativePath = key;
    meta.type = type;
    
    // In a real editor, projectRoot would be available, here we assume relativePath can be resolved later 
    // or pseudoHash is updated in sync()
    meta.contentHash = 0; 

    assetsByID_[meta.id] = meta;
    assetsByPath_[key] = meta.id;

    return meta.id;
}

AssetID AssetRegistry::generateID() {
    static std::random_device rd;
    static std::mt19937_64 gen(rd());
    static std::uniform_int_distribution<uint64_t> dis(2, UINT64_MAX); // 0 and 1 are reserved
    AssetID id;
    do {
        id = dis(gen);
    } while (assetsByID_.find(id) != assetsByID_.end());
    return id;
}

uint64_t AssetRegistry::computeTrueHash(const std::filesystem::path& path) const {
    std::error_code ec;
    uint64_t size = std::filesystem::file_size(path, ec);
    if (ec || size == 0) return 0;

    std::ifstream file(path, std::ios::binary);
    if (!file) return 0;

    uint64_t hash = FNV_OFFSET;
    hash = fnv1a_64(&size, sizeof(size), hash);

    constexpr size_t kChunkSize = 8192;
    char buffer[kChunkSize];

    // Read first 8KB
    file.read(buffer, kChunkSize);
    hash = fnv1a_64(buffer, file.gcount(), hash);

    if (size > kChunkSize * 2) {
        // Read middle 8KB
        file.seekg(size / 2 - kChunkSize / 2, std::ios::beg);
        file.read(buffer, kChunkSize);
        hash = fnv1a_64(buffer, file.gcount(), hash);
    }

    if (size > kChunkSize) {
        // Read last 8KB
        file.seekg(-static_cast<std::streamoff>(std::min<uint64_t>(kChunkSize, size)), std::ios::end);
        file.read(buffer, kChunkSize);
        hash = fnv1a_64(buffer, file.gcount(), hash);
    }

    return hash;
}

void AssetRegistry::loadLocalCache(const std::string& projectRoot) {
    std::filesystem::path path = std::filesystem::path(projectRoot) / kLocalCacheFilename;
    if (!std::filesystem::exists(path)) return;
    std::ifstream file(path);
    if (!file) return;
    try {
        json j; file >> j;
        localCache_.clear();
        for (const auto& [relPath, entryJson] : j.items()) {
            localCache_[relPath] = {
                entryJson.value("ts", 0ULL),
                entryJson.value("sz", 0ULL),
                entryJson.value("h", 0ULL)
            };
        }
    } catch (...) {}
}

void AssetRegistry::saveLocalCache(const std::string& projectRoot) const {
    std::filesystem::path path = std::filesystem::path(projectRoot) / kLocalCacheFilename;
    std::ofstream file(path);
    if (!file) return;
    json j;
    for (const auto& [relPath, entry] : localCache_) {
        j[relPath] = {
            {"ts", entry.lastWriteTime},
            {"sz", entry.fileSize},
            {"h", entry.contentHash}
        };
    }
    file << j.dump(); // compact dump for cache
}

AssetType AssetRegistry::determineType(const std::filesystem::path& path) const {
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    if (ext == ".obj" || ext == ".fbx" || ext == ".gltf" || ext == ".glb") return AssetType::Mesh;
    if (ext == ".png" || ext == ".jpg" || ext == ".bmp") return AssetType::Texture;
    if (ext == ".mat") return AssetType::Material;
    if (ext == ".scene") return AssetType::Scene;
    if (ext == ".ogg") return AssetType::Audio;
    if (ext == ".rig") return AssetType::Rig;
    if (ext == ".anim" || ext == ".bvh" || ext == ".sclip" || ext == ".sgraph" ||
        ext == ".sretarget") return AssetType::Animation;
    if (ext == ".saidafx") return AssetType::Effect;
    return AssetType::Unknown;
}

std::string AssetRegistry::normalizeKey(const std::string& key) const {
    // A key can carry a sub-asset suffix ("model.glb#Run"): only the path
    // part is normalized, the suffix is kept as-is.
    const size_t hash = key.rfind('#');
    std::string pathPart = hash == std::string::npos ? key : key.substr(0, hash);
    const std::string suffix = hash == std::string::npos ? std::string() : key.substr(hash);

    // Asset keys are a portable serialization format. On POSIX,
    // std::filesystem treats '\' as an ordinary character rather than a path
    // separator, so normalize Windows input before asking the host filesystem
    // to collapse "." and ".." components.
    std::replace(pathPart.begin(), pathPart.end(), '\\', '/');
    std::filesystem::path p = std::filesystem::path(pathPart).lexically_normal();
    if (p.is_absolute() && !projectRoot_.empty()) {
        const std::filesystem::path root =
            std::filesystem::path(projectRoot_).lexically_normal();
        const std::filesystem::path rel = p.lexically_relative(root);
        // lexically_relative escapes the root via "..": only relativize
        // paths that are genuinely under the project.
        if (!rel.empty() && rel.begin()->string() != "..") p = rel;
    }

    return p.generic_string() + suffix;
}

} // namespace saida
