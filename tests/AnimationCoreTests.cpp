#include "project/AssetRegistry.hpp"
#include "scene/BVHLoader.hpp"
#include "scene/GLTFLoader.hpp"
#include "scene/animation/AnimBlackboard.hpp"
#include "scene/animation/AnimGraphAsset.hpp"
#include "scene/animation/AnimStateMachine.hpp"
#include "scene/animation/AnimationClip.hpp"
#include "scene/animation/Animator.hpp"
#include "scene/animation/ClipNode.hpp"
#include "scene/animation/ClipView.hpp"
#include "scene/animation/Pose.hpp"
#include "scene/animation/RetargetProfile.hpp"
#include "scene/animation/Rig.hpp"

#include <cassert>
#include <cmath>
#include <memory>
#include <string>

namespace {

bool near(float a, float b) {
    return std::abs(a - b) < 1e-5f;
}

saida::Rig makeOutOfOrderRig() {
    saida::Rig rig;

    saida::Transform child;
    child.position = {0.0f, 2.0f, 0.0f};
    rig.addBone("child", 1, glm::mat4(1.0f), child);

    saida::Transform root;
    root.position = {1.0f, 0.0f, 0.0f};
    rig.addBone("root", -1, glm::mat4(1.0f), root);

    std::string error;
    assert(rig.finalize(&error));
    return rig;
}

void testOutOfOrderHierarchy() {
    saida::Rig rig = makeOutOfOrderRig();
    assert(rig.evaluationOrder().size() == 2);
    assert(rig.evaluationOrder()[0] == 1);
    assert(rig.evaluationOrder()[1] == 0);

    saida::LocalPose pose;
    pose.resize(rig.boneCount());
    for (size_t i = 0; i < rig.boneCount(); ++i)
        pose.localTransforms[i] = rig.bones()[i].restLocal;

    saida::GlobalPose global;
    global.computeFrom(pose, rig);

    assert(near(global.globalMatrices[1][3].x, 1.0f));
    assert(near(global.globalMatrices[1][3].y, 0.0f));
    assert(near(global.globalMatrices[0][3].x, 1.0f));
    assert(near(global.globalMatrices[0][3].y, 2.0f));
}

void testAnimatorUsesRigRestPose() {
    saida::Rig rig = makeOutOfOrderRig();
    saida::Animator animator;
    animator.setRig(&rig);
    animator.onUpdate(0.0f);

    const auto& pose = animator.globalPose();
    assert(pose.globalMatrices.size() == 2);
    assert(near(pose.globalMatrices[0][3].x, 1.0f));
    assert(near(pose.globalMatrices[0][3].y, 2.0f));
}

void testInvalidRigCycle() {
    saida::Rig rig;
    rig.addBone("a", 1, glm::mat4(1.0f));
    rig.addBone("b", 0, glm::mat4(1.0f));

    std::string error;
    assert(!rig.finalize(&error));
    assert(!error.empty());
}

void testStepInterpolation() {
    saida::TypedAnimTrack<glm::vec3> track;
    track.target = saida::TrackTarget::Translation;
    track.interpolation = saida::TrackInterpolation::Step;
    track.timestamps = {0.0f, 1.0f};
    track.values = {{0.0f, 0.0f, 0.0f}, {10.0f, 0.0f, 0.0f}};

    saida::Transform sampled;
    track.evaluate(0.75f, sampled);
    assert(near(sampled.position.x, 0.0f));

    track.evaluate(1.0f, sampled);
    assert(near(sampled.position.x, 10.0f));
}

void testLinearInterpolation() {
    saida::TypedAnimTrack<glm::vec3> track;
    track.target = saida::TrackTarget::Translation;
    track.timestamps = {0.0f, 1.0f};
    track.values = {{0.0f, 0.0f, 0.0f}, {10.0f, 0.0f, 0.0f}};

    saida::Transform sampled;
    track.evaluate(0.5f, sampled);
    assert(near(sampled.position.x, 5.0f));
}

// Golden poses from the versioned glTF fixture: a two-joint skin whose single
// animation exercises LINEAR, STEP and CUBICSPLINE channels. The same file must
// produce the same poses on desktop and web.
void testGltfFixturePoses() {
    saida::GltfAnimationData data;
    std::string error;
    const std::string path = std::string(SAIDA_FIXTURE_DIR) + "/animation/two_bone.gltf";
    assert(saida::GLTFLoader::loadAnimationData(path, data, &error));
    assert(data.rigs.size() == 1);
    assert(data.clips.size() == 1);

    saida::Rig& rig = *data.rigs[0];
    assert(rig.boneCount() == 2);
    assert(rig.findBoneIndex("joint_root") == 0);
    assert(rig.findBoneIndex("joint_child") == 1);
    assert(near(rig.bones()[1].restLocal.position.y, 1.0f));

    const saida::AnimationClip* clip = data.clips[0].get();
    assert(clip->name() == "Mix");
    assert(near(clip->duration(), 1.0f));

    saida::ClipNode node(clip, rig);
    saida::LocalPose bind;
    bind.resize(rig.boneCount());
    for (size_t i = 0; i < rig.boneCount(); ++i)
        bind.localTransforms[i] = rig.bones()[i].restLocal;

    saida::LocalPose local;
    saida::GlobalPose global;

    // t = 0.5 — cubic root at the zero-tangent Hermite midpoint (0.5,0,0), linear
    // child at (0,1.5,0), STEP rotation still holding the first key (identity).
    node.setTime(0.5f);
    node.evaluate(bind, local);
    global.computeFrom(local, rig);
    assert(near(global.globalMatrices[0][3].x, 0.5f));
    assert(near(global.globalMatrices[1][3].x, 0.5f));
    assert(near(global.globalMatrices[1][3].y, 1.5f));
    assert(near(global.globalMatrices[1][0].y, 0.0f));  // X axis unrotated

    // t = 1.0 — root (1,0,0), child (1,2,0), STEP rotation now 90° around Z.
    node.setTime(1.0f);
    node.evaluate(bind, local);
    global.computeFrom(local, rig);
    assert(near(global.globalMatrices[0][3].x, 1.0f));
    assert(near(global.globalMatrices[1][3].x, 1.0f));
    assert(near(global.globalMatrices[1][3].y, 2.0f));
    assert(near(global.globalMatrices[1][0].y, 1.0f));  // X axis rotated onto +Y
}

// Golden values from the versioned BVH fixture (Hips 6 channels, Spine 3).
void testBvhFixtureClip() {
    const std::string path = std::string(SAIDA_FIXTURE_DIR) + "/animation/two_joint.bvh";
    std::unique_ptr<saida::AnimationClip> clip = saida::BVHLoader::parse(path);
    assert(clip);
    assert(near(clip->duration(), 1.0f));  // 3 frames @ 0.5s

    assert(clip->getTracks("Hips") != nullptr);
    assert(clip->getTracks("Spine") != nullptr);
    assert(clip->getTracks("Hips")->size() == 2);   // translation + rotation
    assert(clip->getTracks("Spine")->size() == 1);  // rotation only

    // Hips translation is linear between frames: t=0.25 → halfway to (0,1,0).
    saida::Transform hips;
    for (const auto& track : *clip->getTracks("Hips")) track->evaluate(0.25f, hips);
    assert(near(hips.position.y, 0.5f));

    // Frame 1 (t=0.5): Hips rotated 90° around Z.
    saida::Transform hipsF1;
    for (const auto& track : *clip->getTracks("Hips")) track->evaluate(0.5f, hipsF1);
    const glm::vec3 xAxis = hipsF1.rotation * glm::vec3(1, 0, 0);
    assert(near(xAxis.y, 1.0f));

    // Frame 2 (t=1.0): Spine rotated 90° around Z, Hips back to identity.
    saida::Transform spineF2;
    for (const auto& track : *clip->getTracks("Spine")) track->evaluate(1.0f, spineF2);
    const glm::vec3 spineX = spineF2.rotation * glm::vec3(1, 0, 0);
    assert(near(spineX.y, 1.0f));
}

bool hasDiagnostic(const std::vector<saida::AssetDiagnostic>& diags, const char* code) {
    for (const auto& d : diags)
        if (d.code == code) return true;
    return false;
}

// Sérialisation versionnée : toJson → parse reproduit la vue à l'identique et
// un schéma plus récent que le support courant est refusé proprement.
void testClipViewRoundtrip() {
    saida::ClipView view;
    view.source = "mocap.glb#Take1";
    view.name = "RunLoop";
    view.hasRange = true;
    view.rangeStart = 1.2f;
    view.rangeEnd = 2.05f;
    view.loop = true;
    view.speed = 1.5f;
    view.events.push_back({1.4f, "left_foot_down"});

    auto parsed = saida::ClipView::parse(view.toJson());
    assert(parsed.ok);
    assert(parsed.view.source == view.source);
    assert(parsed.view.name == view.name);
    assert(parsed.view.hasRange);
    assert(near(parsed.view.rangeStart, 1.2f));
    assert(near(parsed.view.rangeEnd, 2.05f));
    assert(near(parsed.view.speed, 1.5f));
    assert(parsed.view.events.size() == 1);
    assert(parsed.view.events[0].name == "left_foot_down");

    nlohmann::json future = view.toJson();
    future["schema"] = saida::kClipViewSchema + 1;
    auto rejected = saida::ClipView::parse(future);
    assert(!rejected.ok);
    assert(hasDiagnostic(rejected.diagnostics, "clipview.schema.unsupported"));

    auto empty = saida::ClipView::parse(nlohmann::json::object());
    assert(!empty.ok);
    assert(hasDiagnostic(empty.diagnostics, "clipview.schema.missing"));
}

// Diagnostics structurés contre une source résolue.
void testClipViewValidation() {
    saida::GltfAnimationData data;
    std::string error;
    const std::string path = std::string(SAIDA_FIXTURE_DIR) + "/animation/two_bone.gltf";
    assert(saida::GLTFLoader::loadAnimationData(path, data, &error));
    const saida::AnimationClip* source = data.clips[0].get();  // duration 1.0

    saida::ClipView view;
    view.source = "two_bone.gltf#Mix";
    view.name = "Bad";
    view.hasRange = true;
    view.rangeStart = 0.5f;
    view.rangeEnd = 0.25f;
    assert(hasDiagnostic(view.validate(source), "clipview.range.reversed"));

    view.rangeEnd = 3.0f;
    assert(hasDiagnostic(view.validate(source), "clipview.range.past_end"));

    view.rangeEnd = 0.75f;
    view.events.push_back({0.9f, "outside"});
    assert(hasDiagnostic(view.validate(source), "clipview.event.out_of_range"));

    view.events.clear();
    assert(view.validate(source).empty());
}

// Deux vues non destructives partagent la même source : aucune copie de clés,
// et la vue joue exactement le segment de la source (mêmes poses aux mêmes
// temps absolus).
void testClipViewsShareSource() {
    saida::GltfAnimationData data;
    std::string error;
    const std::string path = std::string(SAIDA_FIXTURE_DIR) + "/animation/two_bone.gltf";
    assert(saida::GLTFLoader::loadAnimationData(path, data, &error));
    saida::Rig& rig = *data.rigs[0];
    const saida::AnimationClip& source = *data.clips[0];

    saida::ClipView first;
    first.source = "two_bone.gltf#Mix";
    first.name = "FirstHalf";
    first.hasRange = true;
    first.rangeStart = 0.0f;
    first.rangeEnd = 0.5f;

    saida::ClipView second = first;
    second.name = "SecondHalf";
    second.rangeStart = 0.5f;
    second.rangeEnd = 1.0f;

    auto nodeA = first.instantiate(source, rig);
    auto nodeB = second.instantiate(source, rig);
    assert(near(nodeA->time(), 0.0f));
    assert(near(nodeB->time(), 0.5f));

    // La vue boucle dans sa plage : 0.4 + 0.2 s wrappe à 0.1 dans [0, 0.5].
    nodeA->setTime(0.4f);
    nodeA->update(0.2f);
    assert(near(nodeA->time(), 0.1f));

    // Sans boucle, la lecture clampe à la fin de la plage.
    nodeB->setLooping(false);
    nodeB->setTime(0.9f);
    nodeB->update(0.5f);
    assert(near(nodeB->time(), 1.0f));

    // Même temps absolu → même pose que la source directe (clés partagées).
    saida::LocalPose bind;
    bind.resize(rig.boneCount());
    for (size_t i = 0; i < rig.boneCount(); ++i)
        bind.localTransforms[i] = rig.bones()[i].restLocal;

    saida::ClipNode raw(&source, rig);
    raw.setTime(0.75f);
    nodeB->setTime(0.75f);

    saida::LocalPose fromView, fromSource;
    nodeB->evaluate(bind, fromView);
    raw.evaluate(bind, fromSource);
    for (size_t i = 0; i < rig.boneCount(); ++i) {
        assert(near(fromView.localTransforms[i].position.y,
                    fromSource.localTransforms[i].position.y));
        assert(near(fromView.localTransforms[i].position.x,
                    fromSource.localTransforms[i].position.x));
    }
}

// L'asset .sgraph : roundtrip, diagnostics de cohérence, et compilation vers
// une machine d'états jouable qui transitionne sur paramètre.
void testAnimGraphAsset() {
    const std::string path = std::string(SAIDA_FIXTURE_DIR) + "/animation/locomotion.sgraph";
    auto parsed = saida::AnimGraphAsset::loadFile(path);
    assert(parsed.ok);
    assert(parsed.graph.states.size() == 2);
    assert(parsed.graph.validate().empty());

    // Roundtrip : toJson → parse reproduit le graphe.
    auto reparsed = saida::AnimGraphAsset::parse(parsed.graph.toJson());
    assert(reparsed.ok);
    assert(reparsed.graph.initial == "Idle");
    assert(reparsed.graph.transitions.size() == 2);
    assert(reparsed.graph.transitions[0].when.size() == 1);

    // Diagnostics : la fixture cassée accumule les erreurs attendues.
    auto bad = saida::AnimGraphAsset::loadFile(std::string(SAIDA_FIXTURE_DIR) +
                                               "/animation/bad_graph.sgraph");
    assert(bad.ok);  // parse passe, c'est validate() qui rejette
    auto badDiags = bad.graph.validate();
    assert(hasDiagnostic(badDiags, "animgraph.state.unknown_clip"));
    assert(hasDiagnostic(badDiags, "animgraph.initial.unknown"));
    assert(hasDiagnostic(badDiags, "animgraph.transition.unknown_to"));
    assert(hasDiagnostic(badDiags, "animgraph.condition.unknown_param"));

    // Build : le graphe compilé transitionne quand le paramètre change.
    saida::GltfAnimationData data;
    std::string error;
    assert(saida::GLTFLoader::loadAnimationData(
        std::string(SAIDA_FIXTURE_DIR) + "/animation/two_bone.gltf", data, &error));
    const saida::AnimationClip* clip = data.clips[0].get();

    std::vector<saida::AssetDiagnostic> buildDiags;
    auto sm = parsed.graph.build(
        [&](const std::string& key) {
            return key == "two_bone.gltf#Mix" ? clip : nullptr;
        },
        *data.rigs[0], &buildDiags);
    assert(sm);
    assert(buildDiags.empty());

    saida::AnimBlackboard bb;
    sm->setBlackboard(&bb);
    sm->update(0.016f);
    assert(sm->currentState() && sm->currentState()->name() == "Idle");

    bb.setFloat("speed", 1.0f);
    sm->update(0.016f);
    assert(sm->currentState() && sm->currentState()->name() == "Run");

    // Les transitions ne sont pas réévaluées pendant un crossfade : on laisse
    // les 0.2 s se terminer avant de redescendre.
    sm->update(0.3f);
    bb.setFloat("speed", 0.0f);
    sm->update(0.016f);
    assert(sm->currentState() && sm->currentState()->name() == "Idle");

    // Un clip non résolu produit un diagnostic, pas un crash.
    std::vector<saida::AssetDiagnostic> missing;
    auto none = parsed.graph.build([](const std::string&) { return nullptr; },
                                   *data.rigs[0], &missing);
    assert(!none);
    assert(hasDiagnostic(missing, "animgraph.build.clip_unresolved"));
}

// Le profil .sretarget : roundtrip, couverture, auto-map comme suggestion.
void testRetargetProfile() {
    saida::Rig rig;
    rig.addBone("Hips", -1, glm::mat4(1.0f));
    rig.addBone("Spine", 0, glm::mat4(1.0f));
    std::string rigError;
    assert(rig.finalize(&rigError));

    // Clip source aux noms Mixamo : une piste pour Hips seulement.
    saida::AnimationClip clip("mocap", 1.0f);
    auto track = std::make_unique<saida::TypedAnimTrack<glm::vec3>>();
    track->target = saida::TrackTarget::Translation;
    track->timestamps = {0.0f, 1.0f};
    track->values = {{0, 0, 0}, {0, 1, 0}};
    clip.addTrack("mixamorig:Hips", std::move(track));

    // L'auto-map devient un profil éditable.
    auto profile = saida::RetargetProfile::fromAutoMap(rig, clip);
    assert(profile.entries.size() == 1);
    assert(profile.entries[0].first == "Hips");
    assert(profile.entries[0].second == "mixamorig:Hips");

    // Roundtrip.
    auto reparsed = saida::RetargetProfile::parse(profile.toJson());
    assert(reparsed.ok);
    assert(reparsed.profile.toRetargetMap().resolve("Hips") == "mixamorig:Hips");
    assert(reparsed.profile.toRetargetMap().resolve("Spine") == "Spine");  // identité

    // Couverture : Spine n'a pas de piste source → warning, pas erreur.
    auto diags = reparsed.profile.validate(&rig, &clip);
    assert(!saida::hasErrors(diags));
    assert(hasDiagnostic(diags, "retarget.coverage.unmapped_bone"));

    // Erreurs : os inconnu du rig, piste inconnue du clip.
    saida::RetargetProfile broken;
    broken.entries.emplace_back("Tail", "mixamorig:Tail");
    auto brokenDiags = broken.validate(&rig, &clip);
    assert(hasDiagnostic(brokenDiags, "retarget.map.unknown_bone"));
    assert(hasDiagnostic(brokenDiags, "retarget.map.unknown_track"));
}

// Les clés d'assets sont canoniques : un chemin absolu sous la racine projet
// et sa forme relative désignent le même asset, suffixe de sous-asset compris.
void testAssetKeyNormalization() {
    saida::AssetRegistry registry;
    registry.load(SAIDA_FIXTURE_DIR);  // fixe la racine (pas de registre requis)

    const std::string absolute = std::string(SAIDA_FIXTURE_DIR) + "/animation/two_bone.gltf#Mix";
    const saida::AssetID fromAbsolute =
        registry.registerAsset(absolute, saida::AssetType::Animation);
    assert(fromAbsolute != saida::kAssetInvalid);

    // Lookup et ré-enregistrement par la forme relative → même identité.
    assert(registry.getID("animation/two_bone.gltf#Mix") == fromAbsolute);
    assert(registry.registerAsset("animation/two_bone.gltf#Mix",
                                  saida::AssetType::Animation) == fromAbsolute);
    // La clé stockée est portable (relative, séparateurs '/').
    assert(registry.getPath(fromAbsolute) == "animation/two_bone.gltf#Mix");

    // Antislashs Windows normalisés eux aussi.
    assert(registry.getID("animation\\two_bone.gltf#Mix") == fromAbsolute);

    // Un chemin absolu HORS racine reste tel quel (pas de fausse relativité).
    const saida::AssetID outside =
        registry.registerAsset("C:/elsewhere/other.glb#Run", saida::AssetType::Animation);
    assert(registry.getPath(outside) == "C:/elsewhere/other.glb#Run");
}

// Consommation moteur : l'Animator joue une ClipView et un AnimGraphAsset en
// résolvant les clés de sous-assets dans sa bibliothèque de clips.
void testAnimatorPlaysAssets() {
    saida::GltfAnimationData data;
    std::string error;
    assert(saida::GLTFLoader::loadAnimationData(
        std::string(SAIDA_FIXTURE_DIR) + "/animation/two_bone.gltf", data, &error));

    saida::Animator animator;
    animator.setRig(data.rigs[0].get());
    animator.addClip("Mix", data.clips[0].get());

    // playView : la vue devient un état de la FSM interne, plage appliquée.
    saida::ClipView view;
    view.source = "two_bone.gltf#Mix";
    view.name = "SecondHalf";
    view.hasRange = true;
    view.rangeStart = 0.5f;
    view.rangeEnd = 1.0f;

    animator.playView(view, 0.0f);
    assert(animator.currentClip() == "SecondHalf");
    saida::ClipNode* node = animator.activeClipNode();
    assert(node);
    assert(near(node->rangeStart(), 0.5f));
    animator.onUpdate(0.6f);  // 0.5 + 0.6 wrappe à 0.6 dans [0.5, 1.0]
    assert(node->time() >= 0.5f && node->time() <= 1.0f);

    // setGraph : graphe compilé, défauts appliqués, transitions par setFloat.
    auto parsed = saida::AnimGraphAsset::loadFile(std::string(SAIDA_FIXTURE_DIR) +
                                                  "/animation/locomotion.sgraph");
    assert(parsed.ok);
    std::vector<saida::AssetDiagnostic> diags;
    assert(animator.setGraph(parsed.graph, &diags));
    assert(diags.empty());
    assert(near(animator.blackboard().getFloat("speed"), 0.0f));  // défaut appliqué

    auto* sm = dynamic_cast<saida::AnimStateMachine*>(animator.rootNode());
    assert(sm);
    animator.onUpdate(0.016f);
    assert(sm->currentState() && sm->currentState()->name() == "Idle");
    animator.setFloat("speed", 1.0f);
    animator.onUpdate(0.016f);
    assert(sm->currentState() && sm->currentState()->name() == "Run");

    // Sans rig, setGraph échoue avec un diagnostic — pas de crash.
    saida::Animator bare;
    std::vector<saida::AssetDiagnostic> bareDiags;
    assert(!bare.setGraph(parsed.graph, &bareDiags));
    assert(hasDiagnostic(bareDiags, "animgraph.build.no_rig"));
}

} // namespace

int main() {
    testOutOfOrderHierarchy();
    testAnimatorUsesRigRestPose();
    testInvalidRigCycle();
    testStepInterpolation();
    testLinearInterpolation();
    testGltfFixturePoses();
    testBvhFixtureClip();
    testClipViewRoundtrip();
    testClipViewValidation();
    testClipViewsShareSource();
    testAnimGraphAsset();
    testRetargetProfile();
    testAssetKeyNormalization();
    testAnimatorPlaysAssets();
    return 0;
}
