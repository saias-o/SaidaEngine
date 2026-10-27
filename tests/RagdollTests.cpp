// Headless proof of the ragdoll, its component, the two-bone reach and bone
// attachment, on a synthetic T-posed humanoid (metres, facing +Z):
//   - a dropped ragdoll lands, settles on the floor, never sinks through it,
//     and its joints hold: every driven bone read back from the pose sits on
//     its body's joint;
//   - knees and elbows are hinges that bend one way only;
//   - it spawns at rest without its overlapping parts pushing it apart, yet
//     collides with another ragdoll;
//   - a rebase carries every body; despawn returns every body; a full world
//     refuses it whole and an unknown bone is refused by name;
//   - RagdollBehaviour drives an Animator: simulated, frozen when settled,
//     blended back standing; disabling the node takes its bodies out;
//   - TwoBoneIKModifier puts the hand on a target, turns it, bends the elbow
//     toward a pole; BoneAttachment follows the hand the same tick.
#include "behaviours/BoneAttachmentBehaviour.hpp"
#include "behaviours/RagdollBehaviour.hpp"
#include "graphics/Mesh.hpp"
#include "physics/JoltGlue.hpp"
#include "physics/PhysicsWorld.hpp"
#include "physics/CharacterBodyNode.hpp"
#include "physics/CollisionShapeNode.hpp"
#include "physics/Ragdoll.hpp"
#include "scene/Scene.hpp"
#include "scene/animation/Animator.hpp"
#include "scene/animation/PoseModifiers.hpp"

#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/PhysicsSystem.h>

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace saida;

namespace {

int gChecks = 0;

void require(bool condition, const char* what) {
    ++gChecks;
    if (!condition) {
        std::printf("[ragdoll] FAIL: %s\n", what);
        std::fflush(stdout);
        std::exit(1);
    }
}

void requireNear(float actual, float expected, float tolerance, const char* what) {
    ++gChecks;
    if (!(std::fabs(actual - expected) <= tolerance)) {
        std::printf("[ragdoll] FAIL: %s (got %f, expected %f +/- %f)\n", what, actual, expected, tolerance);
        std::fflush(stdout);
        std::exit(1);
    }
}

constexpr float kDt = 1.0f / 60.0f;

struct BoneDef { const char* name; const char* parent; glm::vec3 position; };

// A 1.7 m adult in a T-pose, every bone unrotated at rest.
const std::vector<BoneDef>& skeleton() {
    static const std::vector<BoneDef> bones = {
        {"Root", nullptr, {0, 0, 0}},
        {"Pelvis", "Root", {0, 0.95f, 0}},
        {"Spine", "Pelvis", {0, 1.05f, 0}},
        {"Chest", "Spine", {0, 1.25f, 0}},
        {"Neck", "Chest", {0, 1.45f, 0}},
        {"Head", "Neck", {0, 1.55f, 0}},
        {"LUpperArm", "Chest", {0.2f, 1.42f, 0}},
        {"LForearm", "LUpperArm", {0.48f, 1.42f, 0}},
        {"LHand", "LForearm", {0.74f, 1.42f, 0}},
        {"RUpperArm", "Chest", {-0.2f, 1.42f, 0}},
        {"RForearm", "RUpperArm", {-0.48f, 1.42f, 0}},
        {"RHand", "RForearm", {-0.74f, 1.42f, 0}},
        {"LThigh", "Pelvis", {0.1f, 0.92f, 0}},
        {"LShin", "LThigh", {0.1f, 0.5f, 0}},
        {"LFoot", "LShin", {0.1f, 0.08f, 0}},
        {"RThigh", "Pelvis", {-0.1f, 0.92f, 0}},
        {"RShin", "RThigh", {-0.1f, 0.5f, 0}},
        {"RFoot", "RShin", {-0.1f, 0.08f, 0}},
    };
    return bones;
}

std::unique_ptr<Rig> makeRig() {
    auto rig = std::make_unique<Rig>();
    std::map<std::string, int32_t> index;
    std::map<std::string, glm::vec3> position;
    for (const BoneDef& b : skeleton()) {
        const int32_t parent = b.parent ? index.at(b.parent) : -1;
        Transform rest;
        rest.position = b.position - (b.parent ? position.at(b.parent) : glm::vec3(0.0f));
        rig->addBone(b.name, parent, glm::inverse(glm::translate(glm::mat4(1.0f), b.position)), rest);
        index[b.name] = int32_t(index.size());
        position[b.name] = b.position;
    }
    require(rig->finalize(), "synthetic rig finalizes");
    return rig;
}

HumanoidBones names() {
    HumanoidBones b;
    b.pelvis = "Pelvis"; b.spine = "Spine"; b.chest = "Chest"; b.neck = "Neck"; b.head = "Head";
    b.leftUpperArm = "LUpperArm"; b.leftForearm = "LForearm"; b.leftHand = "LHand";
    b.rightUpperArm = "RUpperArm"; b.rightForearm = "RForearm"; b.rightHand = "RHand";
    b.leftThigh = "LThigh"; b.leftShin = "LShin"; b.leftFoot = "LFoot";
    b.rightThigh = "RThigh"; b.rightShin = "RShin"; b.rightFoot = "RFoot";
    return b;
}

void restPose(const Rig& rig, LocalPose& local, GlobalPose& global) {
    local.resize(rig.boneCount());
    for (size_t i = 0; i < rig.boneCount(); ++i) local.localTransforms[i] = rig.bones()[i].restLocal;
    global.computeFrom(local, rig);
}

void disconnectedRootsAndSmallShapes() {
    PhysicsWorld world;
    Rig rig;
    auto add=[&](const char* name,int parent,glm::vec3 at) {
        Transform rest;rest.position=at;
        rig.addBone(name,parent,glm::inverse(glm::translate(glm::mat4(1),at)),rest);
    };
    add("torso",-1,{0,1,0});add("detached limb",-1,{.2f,1,0});
    require(rig.finalize(),"disconnected skin roots are a valid rig");
    RagdollProfile profile;
    RagdollPart trunk;trunk.bone="torso";trunk.length=.3f;trunk.radius=.1f;profile.parts.push_back(trunk);
    RagdollPart limb;limb.bone="detached limb";limb.length=.3f;limb.radius=.04f;limb.parent="torso";profile.parts.push_back(limb);
    LocalPose local;GlobalPose pose;restPose(rig,local,pose);
    Ragdoll ragdoll(rig,profile);
    require(ragdoll.valid(),"an explicit earlier parent joins disconnected skin roots");
    require(ragdoll.spawn(world,pose,local,glm::mat4(1),{3,2,0}),"disconnected ragdoll spawns");
    for(int i=0;i<30;++i)world.step(kDt);
    LocalPose read;ragdoll.readPose(glm::mat4(1),read);GlobalPose drawn;drawn.computeFrom(read,rig);
    for(size_t i=0;i<2;++i)requireNear(glm::distance(glm::vec3(drawn.globalMatrices[i][3]),ragdoll.bonePosition(i)),0,.001f,
        "every disconnected driven root follows its physical joint position");
    ragdoll.despawn();
    profile.parts[1].parent="missing";
    require(!Ragdoll(rig,profile).valid(),"an unknown explicit parent is refused");
    profile.parts[0].parent="detached limb";
    require(!Ragdoll(rig,profile).valid(),"forward parent references cannot form a cycle");

    Node body;
    for(auto type:{CollisionShapeType::Box,CollisionShapeType::Sphere,CollisionShapeType::Capsule}) {
        auto* shape=body.createChild<CollisionShapeNode>();shape->shapeType=type;
        shape->halfExtents={.002f,.003f,.004f};shape->radius=.002f;shape->height=.008f;
        body.updateTransforms(glm::mat4(1),true);
        auto built=shape->buildShape(glm::mat4(1),body);
        require(built!=nullptr,"millimetre-sized authored primitive builds");
        require(built->GetLocalBounds().GetSize().GetY()<.01f,"explicit small shape has no centimetre inflation");
        body.removeChild(shape);
    }
}

JPH::BodyID addFloor(PhysicsWorld& world) {
    JPH::Ref<JPH::Shape> box = new JPH::BoxShape(JPH::Vec3(50.0f, 0.5f, 50.0f));
    BodyDesc desc;
    desc.shape = box.GetPtr();
    desc.position = {0.0f, -0.5f, 0.0f};  // top at y = 0
    desc.motion = BodyMotion::Static;
    return world.createBody(desc);
}

void stepWorld(PhysicsWorld& world, Ragdoll* ragdoll, float seconds) {
    for (int i = 0; i < int(seconds / kDt); ++i) {
        world.step(kDt);
        if (ragdoll) ragdoll->updateSettled(kDt);
    }
}

size_t partIndex(const RagdollProfile& profile, const std::string& bone) {
    for (size_t i = 0; i < profile.parts.size(); ++i)
        if (profile.parts[i].bone == bone) return i;
    require(false, "profile names the part");
    return 0;
}

// Signed bend of the left knee about +X (positive: shin swung toward -Z, behind).
float kneeBend(const Ragdoll& r, const RagdollProfile& p) {
    const glm::vec3 hip = r.bonePosition(partIndex(p, "LThigh"));
    const glm::vec3 knee = r.bonePosition(partIndex(p, "LShin"));
    const glm::vec3 shinCentre = r.partPosition(partIndex(p, "LShin"));
    const glm::vec3 thigh = glm::normalize(knee - hip), shin = glm::normalize(shinCentre - knee);
    return std::asin(std::clamp(glm::cross(thigh, shin).x, -1.0f, 1.0f));
}

void dropAndSettle(const Rig& rig) {
    PhysicsWorld world;
    addFloor(world);
    const RagdollProfile profile = humanoidRagdoll(names());
    Ragdoll ragdoll(rig, profile);
    require(ragdoll.valid(), "humanoid profile fits the rig");
    require(ragdoll.partCount() == 11, "eleven parts");
    LocalPose local; GlobalPose global;
    restPose(rig, local, global);
    // Tipped forward a little and a metre up, so it falls rather than balances.
    const glm::mat4 placed = glm::translate(glm::mat4(1.0f), {0.0f, 1.0f, 0.0f}) *
                             glm::rotate(glm::mat4(1.0f), 0.25f, glm::vec3(1, 0, 0));
    require(ragdoll.spawn(world, global, local, placed), "spawns");
    require(world.bodyCount() == 12, "eleven bodies beside the floor");
    require(!ragdoll.spawn(world, global, local, placed), "a spawned ragdoll does not spawn twice");
    float lowest = 1e9f;
    for (int i = 0; i < 300; ++i) {
        world.step(kDt);
        ragdoll.updateSettled(kDt);
        for (size_t p = 0; p < ragdoll.partCount(); ++p) lowest = std::min(lowest, ragdoll.partPosition(p).y);
    }
    require(ragdoll.settled(), "settled within five seconds");
    require(lowest > 0.0f, "no part sank through the floor");
    require(ragdoll.rootPosition().y < 0.4f, "the pelvis lies near the floor");
    // Joints hold, and the pose read back puts every driven bone on its joint.
    LocalPose read;
    ragdoll.readPose(placed, read);
    GlobalPose readGlobal;
    readGlobal.computeFrom(read, rig);
    for (size_t p = 0; p < ragdoll.partCount(); ++p) {
        const int32_t bone = rig.findBoneIndex(profile.parts[p].bone);
        const glm::vec3 chain = glm::vec3(placed * glm::vec4(glm::vec3(readGlobal.globalMatrices[size_t(bone)][3]), 1.0f));
        requireNear(glm::length(chain - ragdoll.bonePosition(p)), 0.0f, 0.03f, "driven bone sits on its joint");
    }
    for (const Transform& t : read.localTransforms)
        require(std::isfinite(t.position.x + t.position.y + t.position.z + t.rotation.w + t.rotation.x), "finite pose");
    ragdoll.despawn();
    require(world.bodyCount() == 1, "despawn returns every body");
}

void hingesBendOneWay(const Rig& rig) {
    PhysicsWorld world;
    world.system().SetGravity(JPH::Vec3::sZero());
    const RagdollProfile profile = humanoidRagdoll(names());
    Ragdoll ragdoll(rig, profile);
    LocalPose local; GlobalPose global;
    restPose(rig, local, global);
    require(ragdoll.spawn(world, global, local, glm::mat4(1.0f)), "spawns weightless");
    stepWorld(world, nullptr, 0.5f);
    require(ragdoll.fastestSpeed() < 0.3f, "overlapping parts do not push a resting ragdoll apart");
    const size_t shin = partIndex(profile, "LShin");
    // Kick the foot forward: a knee does not bend that way.
    ragdoll.applyImpulse(shin, {0.0f, 0.0f, 6.0f}, ragdoll.bonePosition(shin) + glm::vec3(0.0f, -0.35f, 0.0f));
    stepWorld(world, nullptr, 0.4f);
    require(kneeBend(ragdoll, profile) > -0.15f, "the knee does not hyperextend forward");
    // Kick it back: it bends.
    ragdoll.applyImpulse(shin, {0.0f, 0.0f, -9.0f}, ragdoll.bonePosition(shin) + glm::vec3(0.0f, -0.35f, 0.0f));
    stepWorld(world, nullptr, 0.4f);
    require(kneeBend(ragdoll, profile) > 0.3f, "the knee bends backward");
}

void ragdollsCollide(const Rig& rig) {
    PhysicsWorld world;
    world.system().SetGravity(JPH::Vec3::sZero());
    const RagdollProfile profile = humanoidRagdoll(names());
    Ragdoll a(rig, profile), b(rig, profile);
    LocalPose local; GlobalPose global;
    restPose(rig, local, global);
    require(a.spawn(world, global, local, glm::mat4(1.0f)), "first spawns");
    require(b.spawn(world, global, local, glm::translate(glm::mat4(1.0f), {0.0f, 0.0f, 0.12f})), "second spawns");
    const float before = glm::length(a.rootPosition() - b.rootPosition());
    stepWorld(world, nullptr, 0.5f);
    require(glm::length(a.rootPosition() - b.rootPosition()) > before + 0.05f, "two ragdolls push each other apart");
    require(a.partOf(a.body(3)) == 3 && a.partOf(b.body(3)) == -1, "partOf tells the ragdolls apart");
}

void rebaseCarriesBodies(const Rig& rig) {
    PhysicsWorld world;
    addFloor(world);
    const RagdollProfile profile = humanoidRagdoll(names());
    Ragdoll ragdoll(rig, profile);
    LocalPose local; GlobalPose global;
    restPose(rig, local, global);
    require(ragdoll.spawn(world, global, local, glm::translate(glm::mat4(1.0f), {0, 0.5f, 0}), {1.0f, 0.0f, 0.0f}),
            "spawns moving");
    stepWorld(world, &ragdoll, 0.2f);
    std::vector<glm::vec3> before;
    for (size_t p = 0; p < ragdoll.partCount(); ++p) before.push_back(ragdoll.partPosition(p));
    const glm::quat turn = glm::angleAxis(1.2f, glm::vec3(0, 1, 0));
    const glm::vec3 shift{1000.0f, 0.0f, -500.0f};
    ragdoll.rebase(shift, turn);
    for (size_t p = 0; p < ragdoll.partCount(); ++p)
        requireNear(glm::length(ragdoll.partPosition(p) - (turn * before[p] + shift)), 0.0f, 2e-3f, "rebase maps every body");
}

void refusals(const Rig& rig) {
    PhysicsCapacity small;
    small.bodies = 6;
    PhysicsWorld world(small);
    addFloor(world);
    Ragdoll ragdoll(rig, humanoidRagdoll(names()));
    LocalPose local; GlobalPose global;
    restPose(rig, local, global);
    require(!ragdoll.spawn(world, global, local, glm::mat4(1.0f)), "a full world refuses the ragdoll");
    require(world.bodyCount() == 1 && !ragdoll.spawned(), "a refused ragdoll leaves no body behind");
    require(world.refusedBodies() > 0, "the refusal is counted");
    HumanoidBones wrong = names();
    wrong.head = "Skull";
    Ragdoll invalid(rig, humanoidRagdoll(wrong));
    require(!invalid.valid() && invalid.error().find("Skull") != std::string::npos, "an unknown bone is refused by name");
}

Node* makeCharacter(Scene& scene, const Rig& rig, Animator*& animator) {
    Node* character = scene.createChild<Node>("character");
    animator = character->addBehaviour<Animator>();
    animator->setRig(const_cast<Rig*>(&rig));
    return character;
}

void behaviourCycle(const Rig& rig) {
    Scene scene;
    PhysicsWorld& world = scene.ensurePhysics();
    addFloor(world);
    Animator* animator = nullptr;
    Node* character = makeCharacter(*scene.createChild<Scene>(), rig, animator);
    auto* ragdoll = character->addBehaviour<RagdollBehaviour>(humanoidRagdoll(names()));
    auto* collider = character->createChild<CharacterBodyNode>();
    auto* shape = collider->createChild<CollisionShapeNode>();
    shape->shapeType=CollisionShapeType::Capsule;shape->radius=.2f;shape->height=1.7f;shape->offset.y=.9f;
    scene.update(kDt);
    const int32_t head = rig.findBoneIndex("Head");
    const float standingHead = animator->globalPose().globalMatrices[size_t(head)][3].y;
    // Thrown sideways and tipping over, as a blow at the shoulders throws it.
    require(ragdoll->activateReplacing(*collider,{2.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -2.5f}), "activates");
    require(collider->physicsSuspended() && world.bodyCount()==12, "capsule immediately gives way to eleven ragdoll parts");
    require(ragdoll->state() == RagdollBehaviour::State::Simulated, "simulated");
    for (int i = 0; i < 360 && ragdoll->state() == RagdollBehaviour::State::Simulated; ++i) scene.update(kDt);
    require(ragdoll->state() == RagdollBehaviour::State::Frozen, "freezes once settled");
    require(world.bodyCount() == 1, "a frozen ragdoll holds no body");
    require(character->transform().position.x > 0.3f, "the node followed the body it threw");
    scene.update(kDt);
    // The node stays upright under the pelvis; the head it draws is near the floor.
    require(character->transform().position.y + animator->globalPose().globalMatrices[size_t(head)][3].y < 0.5f,
            "the drawn pose lies down");
    ragdoll->blendOut(0.5f, 0.0f);
    require(ragdoll->state() == RagdollBehaviour::State::Blending, "blends out");
    requireNear(character->transform().position.y, 0.0f, 1e-4f, "stands on the given ground");
    for (int i = 0; i < 40; ++i) scene.update(kDt);
    require(ragdoll->state() == RagdollBehaviour::State::Animated, "back to the animation");
    require(!collider->physicsSuspended() && world.bodyCount()==2, "recovery restores one capsule");
    requireNear(animator->globalPose().globalMatrices[size_t(head)][3].y, standingHead, 1e-3f, "standing again");

    // Disabled, its bodies leave the world; rebased, they move with the node.
    require(ragdoll->activateReplacing(*collider), "activates again");
    require(world.bodyCount() == 12, "eleven bodies back");
    scene.update(kDt);
    const glm::vec3 before = ragdoll->ragdoll()->rootPosition();
    scene.rebaseSubtree(*character, {250.0f, 0.0f, 0.0f});
    requireNear(ragdoll->ragdoll()->rootPosition().x, before.x + 250.0f, 1e-2f, "a rebase carries the ragdoll");
    character->setEnabled(false);
    scene.refreshHierarchy();  // as for any body, on the scene's next refresh
    require(world.bodyCount() == 1 && ragdoll->state() == RagdollBehaviour::State::Animated, "disabling takes the bodies out");
}

void reachAndAttach(const Rig& rig) {
    Scene scene;
    Animator* animator = nullptr;
    Node* character = makeCharacter(scene, rig, animator);
    auto* ik = animator->addModifier<TwoBoneIKModifier>(rig, TwoBoneIKModifier::Settings{"LUpperArm", "LForearm", "LHand"});
    require(ik->valid(), "arm chain is valid");
    require(!TwoBoneIKModifier(rig, {"LUpperArm", "LHand", "LForearm"}).valid(), "a chain out of parent order is refused");
    Node* prop = character->createChild<Node>("prop");
    Transform offset;
    offset.position = {0.0f, 0.1f, 0.0f};
    auto* attachment = prop->addBehaviour<BoneAttachmentBehaviour>("LHand", offset);
    scene.update(kDt);
    require(attachment->attached(), "attached to the hand");
    const int32_t hand = rig.findBoneIndex("LHand"), elbow = rig.findBoneIndex("LForearm");
    auto handPosition = [&] { return glm::vec3(animator->globalPose().globalMatrices[size_t(hand)][3]); };
    requireNear(glm::length(prop->transform().position - (handPosition() + glm::vec3(0, 0.1f, 0))), 0.0f, 1e-4f,
                "the prop sits on the hand");

    const glm::vec3 target{0.35f, 1.15f, 0.3f};
    const glm::quat turned = glm::angleAxis(0.8f, glm::vec3(0, 0, 1));
    ik->setPole({0.4f, 1.0f, -0.5f});
    ik->setTipRotation(turned);
    ik->setWeightNow(1.0f);
    ik->reach(target);
    scene.update(kDt);
    requireNear(glm::length(handPosition() - target), 0.0f, 2e-3f, "the hand reaches the target");
    const glm::quat handRotation = glm::quat_cast(glm::mat3(animator->globalPose().globalMatrices[size_t(hand)]));
    requireNear(std::abs(glm::dot(glm::normalize(handRotation), turned)), 1.0f, 1e-3f, "the hand turns as asked");
    require(animator->globalPose().globalMatrices[size_t(elbow)][3].z < 0.05f, "the elbow bends toward the pole");
    requireNear(glm::length(prop->transform().position - (handPosition() + turned * glm::vec3(0, 0.1f, 0))), 0.0f, 1e-3f,
                "the prop follows the hand the same tick");

    ik->reach({3.0f, 1.42f, 0.0f});
    scene.update(kDt);
    require(glm::length(handPosition() - glm::vec3(0.2f, 1.42f, 0.0f)) > 0.5f, "out of reach the arm stretches out");
    ik->release();
    for (int i = 0; i < 120; ++i) scene.update(kDt);
    require(!ik->active(), "released, the reach eases out and stops");
    requireNear(glm::length(handPosition() - glm::vec3(0.74f, 1.42f, 0.0f)), 0.0f, 1e-3f, "the hand is back at rest");

    Node* stray = character->createChild<Node>("stray");
    auto* wrong = stray->addBehaviour<BoneAttachmentBehaviour>("Tail");
    scene.update(kDt);
    require(!wrong->attached(), "an unknown bone is not attached");

    // What the renderer skins: a mesh carrying bone weights, never a rigid
    // prop that only sits under the Animator.
    MeshData triangle;
    triangle.vertices.resize(3);
    triangle.vertices[1].pos = {1.0f, 0.0f, 0.0f};
    triangle.vertices[2].pos = {0.0f, 1.0f, 0.0f};
    triangle.indices = {0, 1, 2};
    require(!prepareMesh(triangle)->skinned, "a mesh without bone weights is rigid");
    triangle.vertices[0].boneIndices = {0, -1, -1, -1};
    triangle.vertices[0].boneWeights = {1.0f, 0.0f, 0.0f, 0.0f};
    require(prepareMesh(triangle)->skinned, "a weighted mesh is skinned");
}

} // namespace

int main() {
    disconnectedRootsAndSmallShapes();
    const auto rig = makeRig();
    const std::pair<const char*, void (*)(const Rig&)> cases[] = {
        {"drop and settle", dropAndSettle}, {"hinges", hingesBendOneWay}, {"ragdolls collide", ragdollsCollide},
        {"rebase", rebaseCarriesBodies}, {"refusals", refusals}, {"behaviour", behaviourCycle},
        {"reach and attach", reachAndAttach}};
    for (const auto& [name, run] : cases) {
        run(*rig);
        std::printf("[ragdoll] %s: ok\n", name);
        std::fflush(stdout);
    }
    std::printf("[ragdoll] PASS (%d checks)\n", gChecks);
    return 0;
}
