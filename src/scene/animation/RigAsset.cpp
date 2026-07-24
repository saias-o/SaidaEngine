#include "scene/animation/RigAsset.hpp"

#include <algorithm>
#include <cctype>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <unordered_set>

namespace saida {

namespace {

using json = nlohmann::json;

AssetDiagnostic error(std::string code, std::string path, std::string message) {
    return {std::move(code), AssetDiagnostic::Severity::Error, std::move(path),
            std::move(message)};
}

AssetDiagnostic warning(std::string code, std::string path, std::string message) {
    return {std::move(code), AssetDiagnostic::Severity::Warning, std::move(path),
            std::move(message)};
}

std::string hashToHex(uint64_t hash) {
    char buffer[17];
    std::snprintf(buffer, sizeof(buffer), "%016" PRIx64, hash);
    return buffer;
}

bool hexToHash(const std::string& hex, uint64_t& out) {
    if (hex.empty()) return false;
    return std::sscanf(hex.c_str(), "%" SCNx64, &out) == 1;
}

std::string lowered(const std::string& s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return char(std::tolower(c)); });
    return out;
}

// Standard semantics and name fragments that suggest them. Detection
// distinguishes left/right via the usual markers (left/right, .l/.r, _l/_r).
struct SemanticPattern {
    const char* semantic;
    const char* fragment;
    int side;  // 0 = center, -1 = left, +1 = right
};

constexpr SemanticPattern kSemanticPatterns[] = {
    {"hips", "hips", 0},        {"hips", "pelvis", 0},
    {"spine", "spine", 0},      {"chest", "chest", 0},
    {"neck", "neck", 0},        {"head", "head", 0},
    {"left_upper_arm", "upperarm", -1}, {"right_upper_arm", "upperarm", 1},
    {"left_lower_arm", "forearm", -1},  {"right_lower_arm", "forearm", 1},
    {"left_hand", "hand", -1},  {"right_hand", "hand", 1},
    {"left_upper_leg", "upleg", -1},    {"right_upper_leg", "upleg", 1},
    {"left_lower_leg", "leg", -1},      {"right_lower_leg", "leg", 1},
    {"left_foot", "foot", -1},  {"right_foot", "foot", 1},
};

int sideOf(const std::string& loweredName) {
    if (loweredName.find("left") != std::string::npos) return -1;
    if (loweredName.find("right") != std::string::npos) return 1;
    const auto endsWith = [&](const char* suffix) {
        const size_t n = std::strlen(suffix);
        return loweredName.size() >= n &&
               loweredName.compare(loweredName.size() - n, n, suffix) == 0;
    };
    if (endsWith(".l") || endsWith("_l")) return -1;
    if (endsWith(".r") || endsWith("_r")) return 1;
    return 0;
}

} // namespace

const std::string* RigAsset::boneForSemantic(const std::string& semantic) const {
    for (const auto& [key, bone] : semantics)
        if (key == semantic) return &bone;
    return nullptr;
}

uint64_t RigAsset::skeletonHash(const Rig& rig) {
    uint64_t h = 1469598103934665603ull;
    const auto mix = [&h](const void* data, size_t size) {
        const uint8_t* bytes = static_cast<const uint8_t*>(data);
        for (size_t i = 0; i < size; ++i) {
            h ^= bytes[i];
            h *= 1099511628211ull;
        }
    };
    for (const Bone& bone : rig.bones()) {
        mix(bone.name.data(), bone.name.size());
        mix(&bone.parentIndex, sizeof(bone.parentIndex));
    }
    return h;
}

RigAsset RigAsset::fromRig(const Rig& rig, std::string assetName) {
    RigAsset asset;
    asset.name = std::move(assetName);
    asset.storedHash = skeletonHash(rig);

    for (const SemanticPattern& pattern : kSemanticPatterns) {
        if (asset.boneForSemantic(pattern.semantic)) continue;
        for (const Bone& bone : rig.bones()) {
            const std::string low = lowered(bone.name);
            if (low.find(pattern.fragment) == std::string::npos) continue;
            if (sideOf(low) != pattern.side) continue;
            asset.semantics.emplace_back(pattern.semantic, bone.name);
            break;
        }
    }
    return asset;
}

RigAssetParseResult RigAsset::parse(const nlohmann::json& j) {
    RigAssetParseResult result;
    auto& diags = result.diagnostics;

    if (!j.is_object()) {
        diags.push_back(error("rigasset.root.not_object", "", "document must be a JSON object"));
        return result;
    }
    const int schema = j.value("schema", 0);
    if (schema <= 0) {
        diags.push_back(error("rigasset.schema.missing", "/schema",
                              "'schema' must be a positive integer"));
        return result;
    }
    if (schema > kRigAssetSchema) {
        diags.push_back(error("rigasset.schema.newer", "/schema",
                              "schema " + std::to_string(schema) + " is newer than supported " +
                                  std::to_string(kRigAssetSchema)));
        return result;
    }

    RigAsset& asset = result.asset;
    asset.name = j.value("name", "");

    if (j.contains("semantics")) {
        if (!j["semantics"].is_object()) {
            diags.push_back(error("rigasset.semantics.malformed", "/semantics",
                                  "'semantics' must be an object {semantic: boneName}"));
        } else {
            for (auto it = j["semantics"].begin(); it != j["semantics"].end(); ++it) {
                if (!it.value().is_string() || it.value().get<std::string>().empty()) {
                    diags.push_back(error("rigasset.semantic.malformed",
                                          "/semantics/" + it.key(),
                                          "semantic must map to a non-empty bone name"));
                    continue;
                }
                asset.semantics.emplace_back(it.key(), it.value().get<std::string>());
            }
        }
    }

    if (j.contains("height")) {
        if (!j["height"].is_number() || j["height"].get<float>() < 0.0f) {
            diags.push_back(error("rigasset.height.malformed", "/height",
                                  "'height' must be a non-negative number"));
        } else {
            asset.height = j["height"].get<float>();
        }
    }

    if (j.contains("skeletonHash")) {
        uint64_t hash = 0;
        if (!j["skeletonHash"].is_string() ||
            !hexToHash(j["skeletonHash"].get<std::string>(), hash)) {
            diags.push_back(error("rigasset.hash.malformed", "/skeletonHash",
                                  "'skeletonHash' must be a hex string"));
        } else {
            asset.storedHash = hash;
        }
    }

    result.ok = !hasErrors(diags);
    return result;
}

RigAssetParseResult RigAsset::loadFile(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        RigAssetParseResult result;
        result.diagnostics.push_back(error("rigasset.io.open", "", "cannot open " + path));
        return result;
    }
    json j = json::parse(file, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded()) {
        RigAssetParseResult result;
        result.diagnostics.push_back(error("rigasset.io.json", "", path + " is not valid JSON"));
        return result;
    }
    return parse(j);
}

nlohmann::json RigAsset::toJson() const {
    json j = {{"schema", kRigAssetSchema}, {"name", name}};
    if (!semantics.empty()) {
        json obj = json::object();
        for (const auto& [semantic, bone] : semantics) obj[semantic] = bone;
        j["semantics"] = obj;
    }
    if (height > 0.0f) j["height"] = height;
    if (storedHash != 0) j["skeletonHash"] = hashToHex(storedHash);
    return j;
}

bool RigAsset::saveFile(const std::string& path) const {
    std::ofstream file(path, std::ios::trunc);
    if (!file.is_open()) return false;
    file << toJson().dump(1) << "\n";
    return file.good();
}

std::vector<AssetDiagnostic> RigAsset::validate(const Rig* rig) const {
    std::vector<AssetDiagnostic> diags;

    std::unordered_set<std::string> seen;
    for (const auto& [semantic, bone] : semantics) {
        if (!seen.insert(semantic).second)
            diags.push_back(error("rigasset.semantic.duplicate", "/semantics/" + semantic,
                                  "duplicate semantic '" + semantic + "'"));
        if (rig && rig->findBoneIndex(bone) < 0)
            diags.push_back(error("rigasset.semantic.unknown_bone", "/semantics/" + semantic,
                                  "'" + bone + "' is not a bone of the rig"));
    }
    if (rig && storedHash != 0 && storedHash != skeletonHash(*rig))
        diags.push_back(warning("rigasset.hash.mismatch", "/skeletonHash",
                                "skeleton hash differs from the rig (reimported skeleton?)"));
    return diags;
}

} // namespace saida
