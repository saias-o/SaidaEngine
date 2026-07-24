// Tests du playback production : blend space 1D, couches masquées
// (override/additif), événements traversant une boucle, extraction de root
// motion, triggers/exit time/sync de phase du schéma 2, et LOD de pose
// (control à chaque tick, pose interpolée à fréquence réduite).

#include "scene/animation/AnimGraphAsset.hpp"
#include "scene/animation/AnimStateMachine.hpp"
#include "scene/animation/AnimationClip.hpp"
#include "scene/animation/Animator.hpp"
#include "scene/animation/Blend1DNode.hpp"
#include "scene/animation/ClipNode.hpp"
#include "scene/animation/ClipView.hpp"
#include "scene/animation/LayerNode.hpp"
#include "scene/animation/Rig.hpp"

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

// Rig 3 os : root → spine → arm (chaînes haut du corps testables).
saida::Rig makeRig() {
    saida::Rig rig;
    saida::Transform rest;
    rest.position = {0.0f, 1.0f, 0.0f};
    rig.addBone("root", -1, glm::mat4(1.0f), rest);
    rig.addBone("spine", 0, glm::mat4(1.0f), rest);
    rig.addBone("arm", 1, glm::mat4(1.0f), rest);
    std::string error;
    assert(rig.finalize(&error));
    return rig;
}

// Clip 1 s : translation X du root de 0 à `reach`, linéaire.
std::unique_ptr<saida::AnimationClip> makeRootTravelClip(const char* name, float reach) {
    auto clip = std::make_unique<saida::AnimationClip>(name, 1.0f);
    auto track = std::make_unique<saida::TypedAnimTrack<glm::vec3>>();
    track->target = saida::TrackTarget::Translation;
    track->timestamps = {0.0f, 1.0f};
    track->values = {{0.0f, 1.0f, 0.0f}, {reach, 1.0f, 0.0f}};
    clip->addTrack("root", std::move(track));
    return clip;
}

// Clip constant : rotation du bras de `angle` autour de Z.
std::unique_ptr<saida::AnimationClip> makeArmPoseClip(const char* name, float angle) {
    auto clip = std::make_unique<saida::AnimationClip>(name, 1.0f);
    auto track = std::make_unique<saida::TypedAnimTrack<glm::quat>>();
    track->target = saida::TrackTarget::Rotation;
    track->timestamps = {0.0f, 1.0f};
    const glm::quat q = glm::angleAxis(angle, glm::vec3(0, 0, 1));
    track->values = {q, q};
    clip->addTrack("arm", std::move(track));
    return clip;
}

saida::LocalPose makeBindPose(const saida::Rig& rig) {
    saida::LocalPose pose;
    pose.resize(rig.boneCount());
    for (size_t i = 0; i < rig.boneCount(); ++i)
        pose.localTransforms[i] = rig.bones()[i].restLocal;
    return pose;
}

// Le blend space 1D choisit la paire encadrante et pondère linéairement.
void testBlend1D() {
    saida::Rig rig = makeRig();
    auto idle = makeRootTravelClip("idle", 0.0f);
    auto walk = makeRootTravelClip("walk", 2.0f);
    auto run = makeRootTravelClip("run", 6.0f);

    saida::Blend1DNode blend;
    blend.addInput(4.0f, std::make_unique<saida::ClipNode>(run.get(), rig));
    blend.addInput(0.0f, std::make_unique<saida::ClipNode>(idle.get(), rig));
    blend.addInput(1.5f, std::make_unique<saida::ClipNode>(walk.get(), rig));

    const saida::LocalPose bind = makeBindPose(rig);
    saida::LocalPose pose;

    // Sous le premier seuil et au-delà du dernier : entrée seule.
    blend.setValue(-1.0f);
    blend.update(0.5f);  // toutes les entrées à t=0.5
    blend.evaluate(bind, pose);
    assert(near(pose.localTransforms[0].position.x, 0.0f));

    blend.setValue(10.0f);
    blend.evaluate(bind, pose);
    assert(near(pose.localTransforms[0].position.x, 3.0f));  // run à t=0.5

    // Milieu de [walk=1.5, run=4] → moyenne des deux poses.
    blend.setValue(2.75f);
    blend.evaluate(bind, pose);
    assert(near(pose.localTransforms[0].position.x, 0.5f * (1.0f + 3.0f)));

    // Piloté par blackboard.
    saida::AnimBlackboard bb;
    blend.bindParameter(&bb, "speed");
    bb.setFloat("speed", 1.5f);
    blend.update(0.0f);
    blend.evaluate(bind, pose);
    assert(near(pose.localTransforms[0].position.x, 1.0f));  // walk exact à t=0.5
}

// Couches : override masqué au haut du corps, additif appliqué sur la base.
void testLayerMaskAndAdditive() {
    saida::Rig rig = makeRig();
    auto travel = makeRootTravelClip("travel", 2.0f);
    auto armUp = makeArmPoseClip("arm_up", 1.0f);

    const saida::BoneMask mask = saida::BoneMask::fromChain(rig, "spine");
    assert(near(mask.weightFor(0), 0.0f));
    assert(near(mask.weightFor(1), 1.0f));
    assert(near(mask.weightFor(2), 1.0f));  // descendant de spine

    saida::LayerNode layer;
    layer.setBase(std::make_unique<saida::ClipNode>(travel.get(), rig));
    layer.setOverlay(std::make_unique<saida::ClipNode>(armUp.get(), rig));
    layer.setMask(mask);

    const saida::LocalPose bind = makeBindPose(rig);
    saida::LocalPose pose;
    layer.update(0.5f);
    layer.evaluate(bind, pose);

    // La base traverse (root hors masque), la couche pose le bras.
    assert(near(pose.localTransforms[0].position.x, 1.0f));
    const glm::vec3 armX = pose.localTransforms[2].rotation * glm::vec3(1, 0, 0);
    assert(near(armX.y, std::sin(1.0f), 1e-3f));

    // Additif à mi-poids : la moitié de l'angle par rapport au repos.
    layer.setMode(saida::LayerNode::Mode::Additive);
    layer.setWeight(0.5f);
    layer.evaluate(bind, pose);
    const glm::vec3 halfArmX = pose.localTransforms[2].rotation * glm::vec3(1, 0, 0);
    assert(near(halfArmX.y, std::sin(0.5f), 1e-3f));
}

// Les événements d'une vue sont émis par l'Animator, boucle comprise.
void testClipViewEventsThroughAnimator() {
    saida::Rig rig = makeRig();
    auto clip = makeRootTravelClip("travel", 2.0f);

    saida::Animator animator;
    animator.setRig(&rig);
    animator.addClip("travel", clip.get());

    saida::ClipView view;
    view.source = "synthetic#travel";
    view.name = "Travel";
    view.events.push_back({0.25f, "quarter"});
    view.events.push_back({0.75f, "three_quarters"});

    std::vector<std::string> fired;
    saida::Connection connection =
        animator.animationEvent.connect([&](const std::string& name) { fired.push_back(name); });

    animator.playView(view, 0.0f);
    animator.onUpdate(0.5f);  // 0 → 0.5 : franchit "quarter"
    assert(fired.size() == 1 && fired[0] == "quarter");

    animator.onUpdate(0.6f);  // 0.5 → 1.1 (wrap 0.1) : "three_quarters" seulement
    assert(fired.size() == 2 && fired[1] == "three_quarters");

    animator.onUpdate(0.2f);  // 0.1 → 0.3 : refranchit "quarter" au tour suivant
    assert(fired.size() == 3 && fired[2] == "quarter");
}

// Root motion : le delta extrait cumule la distance parcourue, wrap compris,
// et la pose garde le root à sa translation de repos.
void testRootMotionExtraction() {
    saida::Rig rig = makeRig();
    auto clip = makeRootTravelClip("travel", 2.0f);

    saida::Animator animator;
    animator.setRig(&rig);
    animator.addClip("travel", clip.get());
    animator.setRootMotion(saida::Animator::RootMotionMode::Extract);
    animator.play("travel", true, 0.0f);

    glm::vec3 total(0.0f);
    for (int i = 0; i < 3; ++i) {
        animator.onUpdate(0.5f);
        total += animator.consumeRootMotion();
    }
    // 1.5 s de lecture d'un clip qui parcourt 2 unités/s… le clip parcourt
    // `reach` par boucle de 1 s : 1.5 boucle → 3 unités.
    assert(near(total.x, 3.0f, 1e-3f));

    // La translation du root reste celle du repos dans la pose.
    assert(near(animator.globalPose().globalMatrices[0][3].x, 0.0f, 1e-4f));
}

// Schéma 2 : trigger consommé, exit time d'un one-shot, sync de phase.
void testTriggersExitTimeAndSync() {
    saida::Rig rig = makeRig();
    auto idle = makeRootTravelClip("idle", 0.0f);
    auto attack = makeRootTravelClip("attack", 1.0f);

    const nlohmann::json doc = {
        {"schema", 2},
        {"name", "Combat"},
        {"parameters", {{{"name", "attack"}, {"type", "trigger"}}}},
        {"clips", {{"idle", "synthetic#idle"}, {"attack", "synthetic#attack"}}},
        {"states",
         {{{"name", "Idle"}, {"play", "idle"}},
          {{"name", "Attack"}, {"play", "attack"}, {"loop", false}, {"speed", 2.0f}}}},
        {"initial", "Idle"},
        {"transitions",
         {{{"from", "Idle"}, {"to", "Attack"}, {"when", {{{"param", "attack"}}}}},
          {{"from", "Attack"}, {"to", "Idle"}, {"exitTime", 0.9f}}}}};

    auto parsed = saida::AnimGraphAsset::parse(doc);
    assert(parsed.ok);
    assert(parsed.graph.validate().empty());
    assert(parsed.graph.findParam("attack")->type == saida::AnimParamType::Trigger);

    // Roundtrip schéma 2.
    auto reparsed = saida::AnimGraphAsset::parse(parsed.graph.toJson());
    assert(reparsed.ok);
    assert(near(reparsed.graph.transitions[1].exitTime, 0.9f));
    assert(near(reparsed.graph.findState("Attack")->speed, 2.0f));

    saida::Animator animator;
    animator.setRig(&rig);
    animator.addClip("idle", idle.get());
    animator.addClip("attack", attack.get());
    assert(animator.setGraph(parsed.graph));

    auto* sm = dynamic_cast<saida::AnimStateMachine*>(animator.rootNode());
    assert(sm);
    animator.onUpdate(0.016f);
    assert(sm->currentState()->name() == "Idle");

    // Le trigger lance l'attaque puis est consommé (pas de re-déclenchement).
    animator.setTrigger("attack");
    animator.onUpdate(0.016f);
    assert(sm->currentState()->name() == "Attack");
    assert(near(animator.blackboard().getFloat("attack"), 0.0f));

    // Le one-shot ne revient qu'après l'exit time (0.9 × 1 s à vitesse 2).
    animator.onUpdate(0.2f);
    assert(sm->currentState()->name() == "Attack");
    animator.onUpdate(0.3f);  // phase ≈ (0.016+0.5)*2 ≥ 0.9 → retour
    animator.onUpdate(0.016f);
    assert(sm->currentState()->name() == "Idle");

    // Sync de phase : seekNormalized/normalizedTime, la brique utilisée par
    // les transitions syncPhase.
    saida::ClipNode probe(attack.get(), rig);
    probe.seekNormalized(0.25f);
    assert(near(probe.normalizedTime(), 0.25f));
}

// LOD de pose : à fréquence réduite, la pose interpole entre les deux derniers
// échantillons pendant que le graphe (temps, événements) avance à chaque tick.
void testPoseRateInterpolation() {
    saida::Rig rig = makeRig();
    auto clip = makeRootTravelClip("travel", 2.0f);

    saida::Animator animator;
    animator.setRig(&rig);
    animator.addClip("travel", clip.get());
    animator.play("travel", true, 0.0f);
    animator.setPoseRate(10.0f);  // échantillon de pose toutes les 0.1 s

    animator.onUpdate(0.0f);  // amorce : premier échantillon à t=0
    animator.onUpdate(0.1f);  // nouvel échantillon à t=0.1 (x=0.2)

    // Tick intermédiaire : t=0.15 → interpolation à mi-chemin des poses
    // échantillonnées en 0.1 (x=0.2) et... 0.15 < prochain échantillon.
    animator.onUpdate(0.05f);
    const float x = animator.globalPose().globalMatrices[0][3].x;
    assert(x >= 0.0f && x <= 0.2f + 1e-4f);

    // Sans LOD, la pose colle au temps exact (0.0+0.1+0.05+0.05 = 0.2 s).
    animator.setPoseRate(0.0f);
    animator.onUpdate(0.05f);
    const float exact = animator.globalPose().globalMatrices[0][3].x;
    assert(near(exact, 2.0f * 0.2f, 1e-3f));
}

} // namespace

int main() {
    testBlend1D();
    testLayerMaskAndAdditive();
    testClipViewEventsThroughAnimator();
    testRootMotionExtraction();
    testTriggersExitTimeAndSync();
    testPoseRateInterpolation();
    std::puts("saida_animation_playback_tests: OK");
    return 0;
}
