#pragma once

// RigAsset — the persistent asset for a skeleton (.srig, JSON). It doesn't
// duplicate the hierarchy (that lives in the glTF/BVH source): it carries
// what the source doesn't know — the semantic bone identifiers (hips, spine,
// head, left_hand...), metrics useful for retargeting, and the skeleton's
// compatibility hash. First consumer: the semantic mapping in
// RetargetProfile::fromSemantics.
//
// JSON schema (schema == kRigAssetSchema):
//   {
//     "schema": 1,
//     "name": "Hero",
//     "semantics": { "hips": "mixamorig:Hips", "head": "mixamorig:Head" },
//     "height": 1.8,
//     "skeletonHash": "9f2c..."   // optional: detects a divergent reimport
//   }

#include "scene/animation/ClipView.hpp"  // AssetDiagnostic
#include "scene/animation/Rig.hpp"

#include <nlohmann/json.hpp>

#include <string>
#include <utility>
#include <vector>

namespace saida {

constexpr int kRigAssetSchema = 1;

struct RigAssetParseResult;

class RigAsset {
public:
    static RigAssetParseResult parse(const nlohmann::json& j);
    static RigAssetParseResult loadFile(const std::string& path);

    nlohmann::json toJson() const;
    bool saveFile(const std::string& path) const;

    // Duplicate semantics, bones missing from the rig, divergent hash (warning).
    std::vector<AssetDiagnostic> validate(const Rig* rig) const;

    const std::string* boneForSemantic(const std::string& semantic) const;

    // Stable skeleton fingerprint (names + parents): two rigs with the same
    // hash share their palettes and programs without revalidation.
    static uint64_t skeletonHash(const Rig& rig);

    // Suggested starting point: detects hips/spine/head/hands/feet using
    // common naming conventions. Editable afterward, like the auto-map.
    static RigAsset fromRig(const Rig& rig, std::string assetName);

    std::string name;
    // Pairs (semantic -> bone name), file order preserved.
    std::vector<std::pair<std::string, std::string>> semantics;
    float height = 0.0f;         // 0 = unknown
    uint64_t storedHash = 0;     // 0 = not set
};

struct RigAssetParseResult {
    bool ok = false;
    RigAsset asset;
    std::vector<AssetDiagnostic> diagnostics;
};

} // namespace saida
