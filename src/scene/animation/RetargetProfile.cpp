#include "scene/animation/RetargetProfile.hpp"

#include "scene/animation/AnimationClip.hpp"
#include "scene/animation/Rig.hpp"
#include "scene/animation/RigAsset.hpp"

#include <cmath>
#include <fstream>
#include <unordered_set>

namespace saida {

namespace {

using json = nlohmann::json;

AssetDiagnostic error(std::string code, std::string path, std::string message) {
    return {std::move(code), AssetDiagnostic::Severity::Error,
            std::move(path), std::move(message)};
}

AssetDiagnostic warning(std::string code, std::string path, std::string message) {
    return {std::move(code), AssetDiagnostic::Severity::Warning,
            std::move(path), std::move(message)};
}

} // namespace

RetargetProfileParseResult RetargetProfile::parse(const nlohmann::json& j) {
    RetargetProfileParseResult result;
    auto& diags = result.diagnostics;

    if (!j.is_object()) {
        diags.push_back(error("retarget.root.not_object", "", "document must be a JSON object"));
        return result;
    }

    const int schema = j.value("schema", 0);
    if (schema <= 0) {
        diags.push_back(error("retarget.schema.missing", "/schema",
                              "'schema' must be a positive integer"));
        return result;
    }
    if (schema != kRetargetProfileSchema) {
        diags.push_back(error("retarget.schema.unsupported", "/schema",
                              "unsupported schema " + std::to_string(schema) +
                                  " (expected " +
                                  std::to_string(kRetargetProfileSchema) + ")"));
        return result;
    }

    RetargetProfile& p = result.profile;
    p.name = j.value("name", "");

    if (!j.contains("map") || !j["map"].is_object()) {
        diags.push_back(error("retarget.map.missing", "/map",
                              "'map' must be an object {rigBone: clipTrack}"));
        return result;
    }
    for (auto it = j["map"].begin(); it != j["map"].end(); ++it) {
        if (!it.value().is_string() || it.value().get<std::string>().empty()) {
            diags.push_back(error("retarget.map.malformed", "/map/" + it.key(),
                                  "mapping must be a non-empty source track name"));
            continue;
        }
        p.entries.emplace_back(it.key(), it.value().get<std::string>());
    }

    if (j.contains("corrections")) {
        if (!j["corrections"].is_object()) {
            diags.push_back(error("retarget.corrections.malformed", "/corrections",
                                  "'corrections' must be an object {bone: {preRotation}}"));
        } else {
            for (auto it = j["corrections"].begin(); it != j["corrections"].end(); ++it) {
                const std::string path = "/corrections/" + it.key();
                const json& c = it.value();
                if (!c.is_object() || !c.contains("preRotation") ||
                    !c["preRotation"].is_array() || c["preRotation"].size() != 4) {
                    diags.push_back(error("retarget.correction.malformed", path,
                                          "correction must be {preRotation: [x, y, z, w]}"));
                    continue;
                }
                RetargetBoneCorrection correction;
                correction.bone = it.key();
                const json& q = c["preRotation"];
                correction.preRotation = glm::quat(q[3].get<float>(), q[0].get<float>(),
                                                   q[1].get<float>(), q[2].get<float>());
                p.corrections.push_back(std::move(correction));
            }
        }
    }

    if (j.contains("translationScale")) {
        if (!j["translationScale"].is_number() || j["translationScale"].get<float>() <= 0.0f) {
            diags.push_back(error("retarget.translation_scale.malformed", "/translationScale",
                                  "'translationScale' must be a positive number"));
        } else {
            p.translationScale = j["translationScale"].get<float>();
        }
    }

    result.ok = !hasErrors(diags);
    return result;
}

RetargetProfileParseResult RetargetProfile::loadFile(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        RetargetProfileParseResult result;
        result.diagnostics.push_back(error("retarget.io.open", "", "cannot open " + path));
        return result;
    }
    json j = json::parse(file, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded()) {
        RetargetProfileParseResult result;
        result.diagnostics.push_back(error("retarget.io.json", "", path + " is not valid JSON"));
        return result;
    }
    return parse(j);
}

nlohmann::json RetargetProfile::toJson() const {
    json map = json::object();
    for (const auto& [rigBone, clipTrack] : entries) map[rigBone] = clipTrack;
    json j = {{"schema", kRetargetProfileSchema}, {"name", name}, {"map", map}};

    if (!corrections.empty()) {
        json obj = json::object();
        for (const auto& c : corrections) {
            obj[c.bone] = {{"preRotation",
                            {c.preRotation.x, c.preRotation.y, c.preRotation.z,
                             c.preRotation.w}}};
        }
        j["corrections"] = obj;
    }
    if (translationScale != 1.0f) j["translationScale"] = translationScale;
    return j;
}

bool RetargetProfile::saveFile(const std::string& path) const {
    std::ofstream file(path, std::ios::trunc);
    if (!file.is_open()) return false;
    file << toJson().dump(1) << "\n";
    return file.good();
}

std::vector<AssetDiagnostic> RetargetProfile::validate(const Rig* targetRig,
                                                       const AnimationClip* sourceClip) const {
    std::vector<AssetDiagnostic> diags;

    std::unordered_set<std::string> seen;
    for (const auto& [rigBone, clipTrack] : entries) {
        if (!seen.insert(rigBone).second)
            diags.push_back(error("retarget.map.duplicate", "/map/" + rigBone,
                                  "duplicate mapping for rig bone '" + rigBone + "'"));
        if (targetRig && targetRig->findBoneIndex(rigBone) < 0)
            diags.push_back(error("retarget.map.unknown_bone", "/map/" + rigBone,
                                  "rig has no bone named '" + rigBone + "'"));
        if (sourceClip && !sourceClip->getTracks(clipTrack))
            diags.push_back(error("retarget.map.unknown_track", "/map/" + rigBone,
                                  "source clip has no track '" + clipTrack + "'"));
    }

    // Coverage: rig bones without a track (neither direct nor mapped) — warning.
    if (targetRig && sourceClip) {
        const RetargetMap map = toRetargetMap();
        for (const auto& bone : targetRig->bones()) {
            if (!sourceClip->getTracks(map.resolve(bone.name)))
                diags.push_back(warning("retarget.coverage.unmapped_bone", "/map",
                                        "rig bone '" + bone.name +
                                            "' has no source track (will hold rest pose)"));
        }
    }

    return diags;
}

RetargetMap RetargetProfile::toRetargetMap() const {
    RetargetMap map;
    for (const auto& [rigBone, clipTrack] : entries) map.set(rigBone, clipTrack);
    return map;
}

void RetargetCorrections::apply(LocalPose& pose) const {
    const size_t count = std::min(bones.size(), pose.localTransforms.size());
    for (size_t i = 0; i < count; ++i) {
        const Bone& correction = bones[i];
        if (!correction.active) continue;
        Transform& transform = pose.localTransforms[i];
        transform.rotation = correction.preRotation * transform.rotation;
        if (correction.scaleTranslation)
            transform.position *= translationScale;
        else
            transform.position = correction.restPosition;
    }
}

RetargetCorrections RetargetProfile::compileCorrections(const Rig& targetRig) const {
    RetargetCorrections compiled;
    if (corrections.empty() && translationScale == 1.0f) return compiled;

    compiled.translationScale = translationScale;
    compiled.bones.resize(targetRig.boneCount());
    for (const RetargetBoneCorrection& correction : corrections) {
        const int32_t index = targetRig.findBoneIndex(correction.bone);
        if (index < 0) continue;
        RetargetCorrections::Bone& bone = compiled.bones[size_t(index)];
        bone.active = true;
        bone.preRotation = correction.preRotation;
        // Bone lengths differ between skeletons: only the root keeps its
        // animated translation (scaled), other bones keep their rest one.
        if (targetRig.bones()[size_t(index)].parentIndex < 0) {
            bone.scaleTranslation = true;
        } else {
            bone.restPosition = targetRig.bones()[size_t(index)].restLocal.position;
        }
    }
    return compiled;
}

RetargetProfile RetargetProfile::fromSemantics(const RigAsset& target,
                                               const RigAsset& source) {
    RetargetProfile profile;
    profile.name = target.name + "From" + source.name;
    for (const auto& [semantic, targetBone] : target.semantics) {
        if (const std::string* sourceBone = source.boneForSemantic(semantic))
            profile.entries.emplace_back(targetBone, *sourceBone);
    }
    return profile;
}

void RetargetProfile::computeRestPoseCorrections(const Rig& targetRig, const Rig& sourceRig,
                                                 float targetHeight, float sourceHeight) {
    corrections.clear();
    for (const auto& [targetBone, sourceBone] : entries) {
        const int32_t targetIndex = targetRig.findBoneIndex(targetBone);
        const int32_t sourceIndex = sourceRig.findBoneIndex(sourceBone);
        if (targetIndex < 0 || sourceIndex < 0) continue;

        // The pre-rotation maps the source rest onto the target rest: a clip
        // that leaves the source at rest leaves the target at rest.
        RetargetBoneCorrection correction;
        correction.bone = targetBone;
        correction.preRotation =
            targetRig.bones()[size_t(targetIndex)].restLocal.rotation *
            glm::inverse(sourceRig.bones()[size_t(sourceIndex)].restLocal.rotation);
        corrections.push_back(std::move(correction));
    }

    // Scale: ratio of known heights, otherwise the ratio of the rest
    // positions of the mapped roots (pelvis height), otherwise 1.
    float ratio = 0.0f;
    if (targetHeight > 0.0f && sourceHeight > 0.0f) {
        ratio = targetHeight / sourceHeight;
    } else {
        for (const auto& [targetBone, sourceBone] : entries) {
            const int32_t targetIndex = targetRig.findBoneIndex(targetBone);
            const int32_t sourceIndex = sourceRig.findBoneIndex(sourceBone);
            if (targetIndex < 0 || sourceIndex < 0) continue;
            if (targetRig.bones()[size_t(targetIndex)].parentIndex >= 0) continue;
            const float targetY = targetRig.bones()[size_t(targetIndex)].restLocal.position.y;
            const float sourceY = sourceRig.bones()[size_t(sourceIndex)].restLocal.position.y;
            if (targetY > 0.0f && sourceY > 0.0f) ratio = targetY / sourceY;
            break;
        }
    }
    translationScale = ratio > 0.0f ? ratio : 1.0f;
}

RetargetProfile RetargetProfile::fromAutoMap(const Rig& targetRig,
                                             const AnimationClip& sourceClip) {
    std::vector<std::string> rigBones;
    rigBones.reserve(targetRig.boneCount());
    for (const auto& bone : targetRig.bones()) rigBones.push_back(bone.name);

    const RetargetMap suggested = RetargetMap::autoMap(rigBones, sourceClip.boneNames());

    RetargetProfile profile;
    for (const auto& bone : rigBones) {
        const std::string& track = suggested.resolve(bone);
        if (track != bone) profile.entries.emplace_back(bone, track);
    }
    return profile;
}

} // namespace saida
