#pragma once

// RetargetProfile — the persistent retargeting asset (.sretarget, JSON).
// Schema 1: name-based correspondence (RetargetMap). Schema 2: per-bone rest
// pose corrections (pre-rotation), root translation scale, and semantic
// mapping via two RigAssets. Schema 1 files remain readable as-is (optional
// additional fields).
//
// JSON schema (schema == kRetargetProfileSchema):
//   {
//     "schema": 2,
//     "name": "MixamoToSaida",
//     "map": { "Hips": "mixamorig:Hips", "Spine": "mixamorig:Spine" },
//     "corrections": { "Hips": { "preRotation": [0, 0, 0, 1] } },
//     "translationScale": 1.15
//   }
// Keys are bones of the TARGET rig, values are tracks of the SOURCE clip.

#include "scene/animation/ClipView.hpp"  // AssetDiagnostic
#include "scene/animation/Pose.hpp"
#include "scene/animation/Retarget.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace saida {

class AnimationClip;
class Rig;
class RigAsset;

constexpr int kRetargetProfileSchema = 2;

// Authoring-time correction for a target bone (persisted in the .sretarget).
struct RetargetBoneCorrection {
    std::string bone;
    glm::quat preRotation{1.0f, 0.0f, 0.0f, 0.0f};
};

// Compiled form of the corrections, indexed by target rig bone: applied by
// the Animator on the sampled pose (no name lookup at runtime).
struct RetargetCorrections {
    struct Bone {
        glm::quat preRotation{1.0f, 0.0f, 0.0f, 0.0f};
        bool active = false;
        bool scaleTranslation = false;  // root: source translation x scale
        glm::vec3 restPosition{0.0f};   // other bones: target rest translation
    };
    std::vector<Bone> bones;
    float translationScale = 1.0f;

    bool empty() const { return bones.empty(); }
    void apply(LocalPose& pose) const;
};

struct RetargetProfileParseResult;

class RetargetProfile {
public:
    static RetargetProfileParseResult parse(const nlohmann::json& j);
    static RetargetProfileParseResult loadFile(const std::string& path);

    nlohmann::json toJson() const;
    bool saveFile(const std::string& path) const;

    // Coverage against the target rig and source clip: entries whose bone
    // doesn't exist in the rig, missing source tracks, and rig bones left
    // without a track (warnings — a partially animated rig is valid).
    std::vector<AssetDiagnostic> validate(const Rig* targetRig,
                                          const AnimationClip* sourceClip) const;

    RetargetMap toRetargetMap() const;

    // Corrections compiled for the target rig (indexed by bone). Empty if
    // the profile has neither corrections nor a scale.
    RetargetCorrections compileCorrections(const Rig& targetRig) const;

    // Suggested starting point: the existing name-based auto-mapping,
    // converted into an editable profile (the auto-map becomes a suggestion).
    static RetargetProfile fromAutoMap(const Rig& targetRig, const AnimationClip& sourceClip);

    // Mapping via semantic identifiers shared between two .srig: each
    // semantic present on both sides produces a target -> source entry.
    static RetargetProfile fromSemantics(const RigAsset& target, const RigAsset& source);

    // Fills in the rest pose corrections: pre-rotation = target rest x
    // inverse(source rest) per mapped bone, scale = height ratio (from the
    // .srig if known, otherwise the rest height of the roots).
    void computeRestPoseCorrections(const Rig& targetRig, const Rig& sourceRig,
                                    float targetHeight = 0.0f, float sourceHeight = 0.0f);

    std::string name;
    // Pairs (target rig bone -> source clip track), file order preserved.
    std::vector<std::pair<std::string, std::string>> entries;
    std::vector<RetargetBoneCorrection> corrections;
    float translationScale = 1.0f;
};

struct RetargetProfileParseResult {
    bool ok = false;
    RetargetProfile profile;
    std::vector<AssetDiagnostic> diagnostics;
};

} // namespace saida
