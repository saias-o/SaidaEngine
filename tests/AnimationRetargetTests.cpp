// Tests du retargeting sémantique et du procédural : asset .srig (roundtrip,
// validation, hash, suggestion), mapping par sémantiques partagées,
// corrections de rest pose et d'échelle appliquées par l'Animator, two-bone
// IK et look-at.

#include "scene/animation/AnimationClip.hpp"
#include "scene/animation/Animator.hpp"
#include "scene/animation/ClipNode.hpp"
#include "scene/animation/ProceduralNodes.hpp"
#include "scene/animation/RetargetProfile.hpp"
#include "scene/animation/Rig.hpp"
#include "scene/animation/RigAsset.hpp"

#include <glm/gtc/quaternion.hpp>

#include <cassert>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>

namespace {

bool near(float a, float b, float tolerance = 1e-4f) {
    return std::abs(a - b) < tolerance;
}

bool hasDiagnostic(const std::vector<saida::AssetDiagnostic>& diags, const char* code) {
    for (const auto& d : diags)
        if (d.code == code) return true;
    return false;
}

// Rig cible « Saida » : bassin à 1 m, colonne droite.
saida::Rig makeTargetRig() {
    saida::Rig rig;
    saida::Transform hips;
    hips.position = {0.0f, 1.0f, 0.0f};
    rig.addBone("Hips", -1, glm::mat4(1.0f), hips);
    saida::Transform spine;
    spine.position = {0.0f, 0.5f, 0.0f};
    rig.addBone("Spine", 0, glm::mat4(1.0f), spine);
    std::string error;
    assert(rig.finalize(&error));
    return rig;
}

// Rig source « mocap » : mêmes os sous d'autres noms, bassin à 2 m (double
// échelle) et colonne au repos tournée de 90° autour de Z.
saida::Rig makeSourceRig() {
    saida::Rig rig;
    saida::Transform hips;
    hips.position = {0.0f, 2.0f, 0.0f};
    rig.addBone("mocap:Pelvis", -1, glm::mat4(1.0f), hips);
    saida::Transform spine;
    spine.position = {0.0f, 1.0f, 0.0f};
    spine.rotation = glm::angleAxis(glm::half_pi<float>(), glm::vec3(0, 0, 1));
    rig.addBone("mocap:Chest", 0, glm::mat4(1.0f), spine);
    std::string error;
    assert(rig.finalize(&error));
    return rig;
}

void testRigAsset() {
    saida::Rig rig = makeTargetRig();

    saida::RigAsset asset;
    asset.name = "Hero";
    asset.semantics.emplace_back("hips", "Hips");
    asset.semantics.emplace_back("spine", "Spine");
    asset.height = 1.8f;
    asset.storedHash = saida::RigAsset::skeletonHash(rig);

    // Roundtrip + validation propre.
    auto parsed = saida::RigAsset::parse(asset.toJson());
    assert(parsed.ok);
    assert(parsed.asset.name == "Hero");
    assert(*parsed.asset.boneForSemantic("hips") == "Hips");
    assert(near(parsed.asset.height, 1.8f));
    assert(parsed.asset.storedHash == asset.storedHash);
    assert(parsed.asset.validate(&rig).empty());

    // Os inconnu → erreur ; hash divergent → warning.
    saida::RigAsset broken = asset;
    broken.semantics.emplace_back("head", "Skull");
    assert(hasDiagnostic(broken.validate(&rig), "rigasset.semantic.unknown_bone"));

    saida::RigAsset stale = asset;
    stale.storedHash ^= 1;
    auto staleDiags = stale.validate(&rig);
    assert(!saida::hasErrors(staleDiags));
    assert(hasDiagnostic(staleDiags, "rigasset.hash.mismatch"));

    // Schéma plus récent refusé.
    nlohmann::json future = asset.toJson();
    future["schema"] = saida::kRigAssetSchema + 1;
    assert(!saida::RigAsset::parse(future).ok);

    // Suggestion par conventions de nommage (côté gauche/droite compris).
    saida::Rig mixamo;
    mixamo.addBone("mixamorig:Hips", -1, glm::mat4(1.0f));
    mixamo.addBone("mixamorig:LeftHand", 0, glm::mat4(1.0f));
    mixamo.addBone("mixamorig:RightHand", 0, glm::mat4(1.0f));
    std::string error;
    assert(mixamo.finalize(&error));
    const saida::RigAsset guessed = saida::RigAsset::fromRig(mixamo, "Mixamo");
    assert(*guessed.boneForSemantic("hips") == "mixamorig:Hips");
    assert(*guessed.boneForSemantic("left_hand") == "mixamorig:LeftHand");
    assert(*guessed.boneForSemantic("right_hand") == "mixamorig:RightHand");
}

void testSemanticMappingAndCorrections() {
    saida::Rig target = makeTargetRig();
    saida::Rig source = makeSourceRig();

    saida::RigAsset targetAsset;
    targetAsset.name = "Saida";
    targetAsset.semantics.emplace_back("hips", "Hips");
    targetAsset.semantics.emplace_back("spine", "Spine");

    saida::RigAsset sourceAsset;
    sourceAsset.name = "Mocap";
    sourceAsset.semantics.emplace_back("hips", "mocap:Pelvis");
    sourceAsset.semantics.emplace_back("spine", "mocap:Chest");
    sourceAsset.semantics.emplace_back("head", "mocap:Head");  // absent côté cible

    // Mapping par sémantiques partagées.
    saida::RetargetProfile profile =
        saida::RetargetProfile::fromSemantics(targetAsset, sourceAsset);
    assert(profile.entries.size() == 2);
    assert(profile.toRetargetMap().resolve("Hips") == "mocap:Pelvis");
    assert(profile.toRetargetMap().resolve("Spine") == "mocap:Chest");

    // Corrections de rest pose : échelle 0.5 (bassins 1 m vs 2 m) et
    // pré-rotation de -90° sur la colonne.
    profile.computeRestPoseCorrections(target, source);
    assert(near(profile.translationScale, 0.5f));

    // Roundtrip schéma 2 (corrections + échelle persistées) ; un document
    // schéma 1 reste lisible.
    auto reparsed = saida::RetargetProfile::parse(profile.toJson());
    assert(reparsed.ok);
    assert(reparsed.profile.corrections.size() == 2);
    assert(near(reparsed.profile.translationScale, 0.5f));

    nlohmann::json wrongSchema = {{"schema", 1}, {"name", "Old"}, {"map", {{"Hips", "pelvis"}}}};
    assert(!saida::RetargetProfile::parse(wrongSchema).ok);

    // Application par l'Animator : le clip source anime la colonne à SA rest
    // pose (90°) et translate le bassin de 2 m — la cible doit rester à sa
    // propre rest pose (0°) avec un bassin translaté de 1 m.
    saida::AnimationClip clip("mocap_idle", 1.0f);
    auto hipsTrack = std::make_unique<saida::TypedAnimTrack<glm::vec3>>();
    hipsTrack->target = saida::TrackTarget::Translation;
    hipsTrack->timestamps = {0.0f, 1.0f};
    hipsTrack->values = {{0.0f, 2.0f, 0.0f}, {2.0f, 2.0f, 0.0f}};
    clip.addTrack("mocap:Pelvis", std::move(hipsTrack));
    auto spineTrack = std::make_unique<saida::TypedAnimTrack<glm::quat>>();
    spineTrack->target = saida::TrackTarget::Rotation;
    spineTrack->timestamps = {0.0f, 1.0f};
    const glm::quat sourceRest = glm::angleAxis(glm::half_pi<float>(), glm::vec3(0, 0, 1));
    spineTrack->values = {sourceRest, sourceRest};
    clip.addTrack("mocap:Chest", std::move(spineTrack));

    saida::Animator animator;
    animator.setRig(&target);
    animator.addClip("mocap_idle", &clip);
    animator.setRetargetProfile(profile);
    animator.play("mocap_idle", true, 0.0f);
    animator.onUpdate(0.5f);

    // Bassin : translation source (1, 2, 0) × 0.5 = (0.5, 1, 0).
    assert(near(animator.globalPose().globalMatrices[0][3].x, 0.5f, 1e-3f));
    assert(near(animator.globalPose().globalMatrices[0][3].y, 1.0f, 1e-3f));
    // Colonne : la rotation « repos source » corrigée redevient le repos cible
    // (identité) — l'axe X local reste X monde.
    const glm::mat4& spineGlobal = animator.globalPose().globalMatrices[1];
    assert(near(spineGlobal[0].x, 1.0f, 1e-3f));
    assert(near(spineGlobal[0].y, 0.0f, 1e-3f));
}

void testTwoBoneIK() {
    // Bras plié à plat : épaule (0,0,0) → coude (1,0,0) → main (2,0,0).
    saida::Rig rig;
    saida::Transform shoulder;
    rig.addBone("shoulder", -1, glm::mat4(1.0f), shoulder);
    saida::Transform elbow;
    elbow.position = {1.0f, 0.0f, 0.0f};
    rig.addBone("elbow", 0, glm::mat4(1.0f), elbow);
    saida::Transform hand;
    hand.position = {1.0f, 0.0f, 0.0f};
    rig.addBone("hand", 1, glm::mat4(1.0f), hand);
    std::string error;
    assert(rig.finalize(&error));

    saida::LocalPose bind;
    bind.resize(rig.boneCount());
    for (size_t i = 0; i < rig.boneCount(); ++i)
        bind.localTransforms[i] = rig.bones()[i].restLocal;

    saida::TwoBoneIKNode ik(rig, "shoulder", "elbow", "hand");
    assert(ik.valid());
    ik.setPole({1.0f, 1.0f, 0.0f});  // coude vers le haut

    saida::LocalPose pose;
    saida::GlobalPose global;
    for (const glm::vec3 target :
         {glm::vec3(1.2f, 0.8f, 0.0f), glm::vec3(0.5f, -1.0f, 0.5f), glm::vec3(1.8f, 0.0f, 0.2f)}) {
        ik.setTarget(target);
        ik.evaluate(bind, pose);
        global.computeFrom(pose, rig);
        const glm::vec3 tip = glm::vec3(global.globalMatrices[2][3]);
        assert(glm::length(tip - target) < 1e-3f);
    }

    // Cible hors de portée : la chaîne s'étend vers elle sans l'atteindre.
    ik.setTarget({5.0f, 0.0f, 0.0f});
    ik.evaluate(bind, pose);
    global.computeFrom(pose, rig);
    const glm::vec3 tip = glm::vec3(global.globalMatrices[2][3]);
    assert(near(glm::length(tip), 2.0f, 1e-2f));  // bras tendu
    assert(near(tip.x, 2.0f, 1e-2f));
}

void testLookAt() {
    saida::Rig rig;
    saida::Transform head;
    head.position = {0.0f, 1.0f, 0.0f};
    rig.addBone("head", -1, glm::mat4(1.0f), head);
    std::string error;
    assert(rig.finalize(&error));

    saida::LocalPose bind;
    bind.resize(1);
    bind.localTransforms[0] = rig.bones()[0].restLocal;

    saida::LookAtNode lookAt(rig, "head", {0.0f, 0.0f, 1.0f});
    assert(lookAt.valid());
    lookAt.setTarget({1.0f, 1.0f, 0.0f});  // à droite de la tête, même hauteur

    saida::LocalPose pose;
    lookAt.evaluate(bind, pose);
    const glm::vec3 forward = pose.localTransforms[0].rotation * glm::vec3(0, 0, 1);
    assert(near(forward.x, 1.0f, 1e-3f));
    assert(near(forward.y, 0.0f, 1e-3f));

    // Poids nul : la pose d'entrée passe intacte.
    lookAt.setWeight(0.0f);
    lookAt.evaluate(bind, pose);
    const glm::vec3 rest = pose.localTransforms[0].rotation * glm::vec3(0, 0, 1);
    assert(near(rest.z, 1.0f));
}

} // namespace

int main() {
    testRigAsset();
    testSemanticMappingAndCorrections();
    testTwoBoneIK();
    testLookAt();
    std::puts("saida_animation_retarget_tests: OK");
    return 0;
}
