#include "scene/Scene.hpp"
#include "core/Camera.hpp"
#include "nodes/CameraNode.hpp"
#include "render/CameraDirector.hpp"
#include "core/Window.hpp"
#include "graphics/VulkanDevice.hpp"
#include "graphics/Mesh.hpp"
#include "graphics/ResourceManager.hpp"
#include "graphics/Buffer.hpp"
#include "graphics/Texture.hpp"
#include "core/PngWriter.hpp"
#include "render/EnvironmentSH.hpp"
#include "graphics/GeometryRegistry.hpp"
#include "graphics/MeshCache.hpp"
#include "graphics/GpuGraveyard.hpp"
#include "graphics/PipelineBatch.hpp"
#include "core/Paths.hpp"
#include "core/Profiler.hpp"
#include "core/Time.hpp"
#include "render/PostProcessor.hpp"
#include "scene/GLTFLoader.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include "authoring/SceneSnapshot.hpp"
#include "nodes/MeshNode.hpp"
#include "nodes/UIImageNode.hpp"
#include "nodes/TerrainRingsNode.hpp"
#include "behaviours/LODGroupBehaviour.hpp"
#include "physics/CollisionShapeNode.hpp"
#include "physics/RigidBodyNode.hpp"
#include "physics/StaticBodyNode.hpp"
#include "physics/CharacterBodyNode.hpp"
#include "physics/JointNodes.hpp"
#include "physics/PhysicsWorld.hpp"
#include <nlohmann/json.hpp>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <stdexcept>
#include <filesystem>
#include <chrono>
#include <thread>
#include <cstring>
#include <limits>

using namespace saida;
namespace {
int checks = 0;
void require(bool ok, const char* message) {
    ++checks;
    if (!ok) { std::printf("[streaming] FAIL: %s\n", message); std::abort(); }
}
void near(glm::vec3 actual, glm::vec3 expected, const char* message) {
    require(glm::length(actual - expected) < .002f, message);
}
struct Counter : Behaviour {
    int updates = 0, steps = 0;
    void onUpdate(float) override { ++updates; }
    // Substep callbacks are opt-in (Behaviour::hasPhysicsStep).
    bool hasPhysicsStep() const override { return true; }
    void onPhysicsStep(float dt) override {
        require(std::abs(dt - PhysicsWorld::kFixedStep) < 1e-7f, "fixed callback duration");
        ++steps;
    }
};

void indexChanges() {
    Scene scene, unrelated;
    Node* branch = nullptr;
    MeshNode* leaf = nullptr;
    for (int i = 0; i < 32; ++i) {
        branch = scene.createChild<Node>();
        for (int j = 0; j < 64; ++j) leaf = branch->createChild<MeshNode>();
    }
    auto* counter = leaf->addBehaviour<Counter>();
    scene.update(.01f);
    require(scene.meshes().size() == 2048, "initial scene membership");
    scene.refreshHierarchy();
    require(scene.indexedNodesLastRefresh() == 0, "stable scene does no index work");
    unrelated.createChild<Node>();
    scene.refreshHierarchy();
    require(scene.indexedNodesLastRefresh() == 0, "other scenes do not invalidate this scene");
    leaf->setVisible(false);
    scene.refreshHierarchy();
    require(scene.indexedNodesLastRefresh() == 3, "leaf visibility reindexes only ancestor path");
    require(scene.meshes().size() == 2047, "hidden mesh removed from render list");
    scene.update(.01f);
    require(counter->updates == 2, "hidden nodes still simulate");
    branch->setEnabled(false);
    scene.update(.01f);
    require(counter->updates == 2, "disabled branches do not simulate");
    require(scene.meshes().size() == 1984, "disabled descendants removed");
    branch->setEnabled(true);
    leaf->setVisible(true);
    scene.refreshHierarchy();
    require(scene.meshes().size() == 2048, "reactivation restores membership");
    leaf->setMeshEnabled(false);
    scene.refreshHierarchy();
    require(scene.meshes().size() == 2047, "meshEnabled invalidates membership");
    branch->clearChildren();
    scene.refreshHierarchy();
    require(scene.meshes().size() == 1984, "clearing children removes cached pointers");
}

void ownershipAndReparenting() {
    Scene scene;
    auto* first = scene.createChild<Node>();
    auto* second = scene.createChild<Node>();
    auto* a = first->createChild<UIImageNode>();
    auto* b = second->createChild<UIImageNode>();
    a->setTexture(101); b->setTexture(101);
    scene.refreshHierarchy();
    const auto version = scene.resourceVersion();
    first->setEnabled(false); second->setVisible(false);
    scene.refreshHierarchy();
    require(scene.resourceUsage().textures.count(101) == 1, "disabled and hidden branches retain assets");
    require(scene.resourceVersion() == version, "visibility does not rebuild resource ownership");
    a->setTexture(102);
    scene.refreshHierarchy();
    require(scene.resourceUsage().textures.size() == 2, "resource setter updates ownership without topology change");
    first->addChild(second->detachChild(b));
    scene.refreshHierarchy();
    require(scene.containsNode(b) && scene.resourceUsage().textures.count(101), "reparent to earlier branch retains assets");
    second->addChild(first->detachChild(b));
    scene.refreshHierarchy();
    require(scene.containsNode(b) && scene.resourceUsage().textures.count(101), "reparent to later branch retains assets");
    second->removeChild(b);
    scene.refreshHierarchy();
    require(!scene.resourceUsage().textures.count(101), "last resource reference removed");
    scene.settings().skyboxTexture = 103;
    scene.refreshHierarchy();
    require(scene.resourceUsage().textures.count(103), "mutable scene settings retain skybox");
    scene.clearChildren();
    scene.refreshHierarchy();
    require(scene.resourceUsage().textures.size() == 1, "clear removes resources but preserves skybox");
}

void transformAndCameraContract() {
    Scene scene;
    auto* frame = scene.createChild<Node>();
    frame->transform().position = {20.f, -4.f, 7.f};
    frame->transform().rotation = glm::angleAxis(.7f, glm::normalize(glm::vec3(1, 2, 3)));
    frame->transform().scale = {-2.f, .5f, 3.f};
    const auto& t = frame->transform();
    const auto reference = glm::scale(glm::translate(glm::mat4(1.f), t.position) *
                                    glm::mat4_cast(t.rotation), t.scale);
    for (int column = 0; column < 4; ++column)
        require(glm::length(t.matrix()[column] - reference[column]) < 1e-6f,
                "affine TRS preserves rotation, negative/nonuniform scale and translation");
    auto* first = frame->createChild<CameraNode>();
    first->transform().position = {1, 2, 3};
    first->priority = -3;
    first->setVisible(false);  // visibility does not disable a camera
    auto* second = scene.createChild<CameraNode>();
    second->priority = -3;
    second->transform().position = {50, 0, 0};
    auto* disabled = scene.createChild<Node>();
    disabled->setEnabled(false);
    disabled->createChild<CameraNode>()->priority = 100;
    Camera out;
    CameraDirector director;
    director.blendDuration = 0.f;
    require(director.update(scene, out, .01f), "camera selected without a prior scene update");
    near(out.position, glm::vec3(reference * glm::vec4(1, 2, 3, 1)),
         "priority tie follows hierarchy order and hidden camera inherits parent TRS");
    frame->transform().position.x += 4.f;
    director.update(scene, out, .01f);
    near(out.position, glm::vec3(frame->localMatrix() * glm::vec4(1, 2, 3, 1)),
         "direct ancestor transform edit reaches camera immediately");
    second->priority = 2;
    director.update(scene, out, .01f);
    near(out.position, {50, 0, 0}, "mutable priority changes camera selection");
    second->active = false;
    frame->setEnabled(false);
    require(!director.update(scene, out, .01f), "inactive cameras and disabled ancestors leave no live camera");
    disabled->setEnabled(true);
    require(director.update(scene, out, .01f), "reactivating a camera branch restores selection");
    scene.clearChildren();
    require(!director.update(scene, out, .01f), "removed cameras leave no stale selection");
    Node outer;
    outer.transform().position = {100, 0, 0};
    auto* embedded = outer.createChild<Scene>();
    embedded->createChild<CameraNode>()->transform().position = {2, 3, 4};
    require(director.update(*embedded, out, .01f), "camera in an embedded scene is selected");
    near(out.position, {2, 3, 4}, "camera pose stays relative to the selected scene root");
}

void fixedStepsAndRebase() {
    Scene scene;
    auto* frame = scene.createChild<Node>();
    frame->transform().position = {20.f, 0.f, 0.f};
    auto* body = frame->createChild<RigidBodyNode>();
    body->gravityFactor = 0.f;
    body->linearDamping = body->angularDamping = 0.f;
    body->transform().position = {1.f, 2.f, 3.f};
    auto* shape = body->createChild<CollisionShapeNode>();
    shape->shapeType = CollisionShapeType::Box;
    shape->halfExtents = glm::vec3(.5f);
    auto* counter = body->addBehaviour<Counter>();
    scene.update(PhysicsWorld::kFixedStep * .5f);
    require(counter->steps == 0, "no physics callback before accumulator reaches a step");
    scene.update(PhysicsWorld::kFixedStep * 2.5f);
    require(counter->steps == 3, "callback runs for each actual substep");
    auto* physics = scene.physics();
    const auto id = body->bodyId();
    const glm::vec3 velocity{2.f, 0.f, 1.f}, spin{.1f, .2f, .3f};
    physics->setLinearVelocity(id, velocity);
    physics->setAngularVelocity(id, spin);
    const auto position = glm::vec3(body->worldTransform()[3]);
    const glm::quat turn = glm::angleAxis(glm::radians(90.f), glm::vec3(0, 1, 0));
    const glm::vec3 offset{-100.f, 4.f, 7.f};
    scene.rebaseSubtree(*frame, offset, turn);
    glm::vec3 p; glm::quat q;
    physics->getBodyTransform(id, p, q);
    near(p, turn * position + offset, "physics pose follows the new frame");
    near(glm::vec3(body->worldTransform()[3]), p, "render and solver agree after rebase");
    near(physics->linearVelocity(id), turn * velocity, "linear velocity rotates with frame");
    near(physics->angularVelocity(id), turn * spin, "angular velocity rotates with frame");
    scene.update(PhysicsWorld::kFixedStep);
    require(body->bodyId() == id, "rebase preserves body identity");
    near(glm::vec3(body->worldTransform()[3]), p + turn * velocity * PhysicsWorld::kFixedStep,
         "next solver step does not undo the rebase");
    frame->transform().scale = {1.f, 2.f, 1.f};
    bool refused = false;
    try { scene.rebaseSubtree(*body, {1, 0, 0}); }
    catch (const std::invalid_argument&) { refused = true; }
    require(refused, "unsupported parent scale fails explicitly");
}

void rebaseDestinationPrecision() {
    Scene scene;
    auto* root=scene.createChild<Node>();
    root->transform().position={-199832.390625f,-2076780.5f,-4689437.5f};
    root->transform().rotation=glm::angleAxis(.833f,glm::vec3(1,0,0))*glm::angleAxis(.0464f,glm::vec3(0,0,1));
    auto* body=root->createChild<StaticBodyNode>();
    auto* shape=body->createChild<CollisionShapeNode>();
    shape->shapeType=CollisionShapeType::Box;shape->halfExtents={.25f,.5f,.75f};shape->offset={12.f,222.f,35.f};
    scene.update(1.f/60.f);
    const auto id=body->bodyId();
    const glm::vec3 destination{.01465f,-413.545f,-210.752f};
    const auto orientation=glm::angleAxis(.012f,glm::vec3(0,1,0));
    scene.rebaseSubtreeTo(*root,destination,orientation);
    require(glm::length(glm::vec3(root->worldTransform()[3])-destination)<1e-4f,
            "explicit rebase keeps a precise near destination after a distant rotation");
    glm::vec3 p;glm::quat q;scene.physics()->getBodyTransform(id,p,q);
    near(p,destination,"solver root shares the precise near destination");
    const auto probe=destination+orientation*shape->offset;
    const auto hit=scene.physics()->raycast(probe+glm::vec3(0,2,0),{0,-1,0},4.f);
    require(hit.hit && hit.body==id && std::abs(hit.distance-1.5f)<.002f,
            "precise rebase keeps the collider at its drawn local offset");
    scene.update(1.f/60.f);
    require(body->bodyId()==id,"explicit rebase preserves body identity after synchronization");
    auto* parent=scene.createChild<Node>();parent->addChild(scene.detachChild(root));
    parent->transform().position={3.f,7.f,11.f};
    parent->transform().rotation=glm::angleAxis(.2f,glm::vec3(0,1,0));
    scene.rebaseSubtreeTo(*root,destination,orientation);
    near(glm::vec3(root->worldTransform()[3]),destination,"explicit rebase reads a directly mutated parent frame");
}

void rebaseCharacterAndJoint() {
    Scene scene;
    auto* anchor = scene.createChild<RigidBodyNode>();
    anchor->transform().position = {10, 5, 0};
    auto* box = anchor->createChild<CollisionShapeNode>();
    box->shapeType = CollisionShapeType::Box; box->halfExtents = glm::vec3(.5f);
    auto* joint = anchor->createChild<FixedJointNode>();
    auto* character = scene.createChild<CharacterBodyNode>();
    character->transform().position = {0, 10, 0};
    auto* capsule = character->createChild<CollisionShapeNode>();
    capsule->shapeType = CollisionShapeType::Capsule;
    capsule->radius = .4f; capsule->height = 1.8f;
    scene.update(PhysicsWorld::kFixedStep);
    require(joint->jointActive(), "world joint created");
    const auto anchorId = anchor->bodyId(), characterId = character->innerBodyId();
    const auto oldCharacter = glm::vec3(character->worldTransform()[3]);
    const auto oldAnchor = glm::vec3(anchor->worldTransform()[3]);
    character->velocity = {1, 0, 2};
    const glm::vec3 offset{-300, 0, 50};
    const auto rotation = glm::angleAxis(.5f, glm::vec3(0, 1, 0));
    scene.rebaseOrigin(offset, rotation);
    require(!joint->jointActive(), "world joint invalidated on origin change");
    near(glm::vec3(character->worldTransform()[3]), rotation * oldCharacter + offset,
         "character pose follows origin change");
    near(character->velocity, rotation * glm::vec3(1, 0, 2), "character velocity rotates");
    scene.update(PhysicsWorld::kFixedStep);
    require(joint->jointActive(), "world joint rebuilt in new frame");
    require(anchor->bodyId() == anchorId && character->innerBodyId() == characterId,
            "origin change preserves rigid and character identities");
    near(glm::vec3(anchor->worldTransform()[3]), rotation * oldAnchor + offset,
         "joint pins its body in the new frame");
}

void groupContract() {
    LODGroupBehaviour group;
    group.setLevels({{"Near", .1f}, {"Far", 0.f}});
    nlohmann::json doc;
    group.save(doc);
    LODGroupBehaviour restored;
    restored.load(doc);
    require(restored.levels().size() == 2 && restored.levels()[0].path == "Near",
            "child LOD levels survive serialization");
    Scene original, roundtrip;
    auto* root = original.createChild<Node>("Tree");
    root->createChild<Node>("Near"); root->createChild<Node>("Far");
    root->addBehaviour<LODGroupBehaviour>()->setLevels(restored.levels());
    const auto snapshot = authoring::serializeSceneSnapshot(original, nullptr);
    std::string error;
    require(authoring::deserializeSceneSnapshot(snapshot, roundtrip, &error), "child LOD snapshot loads");
    auto* roundtripGroup = roundtrip.findByPath("Tree")->getBehaviour<LODGroupBehaviour>();
    require(roundtripGroup && roundtripGroup->levels().size() == 2, "headless authoring preserves child LOD data");
    bool refused = false;
    try { group.setLevels({{"Near", .1f}, {"Far", .2f}}); }
    catch (const std::invalid_argument&) { refused = true; }
    require(refused, "increasing LOD thresholds refused");
    {
        // Coverage is how far, not where in the frame: turning the camera on
        // the spot changes no level.
        const Aabb box{glm::vec3(-1.f), glm::vec3(1.f)};
        const glm::mat4 proj = glm::perspective(glm::radians(60.f), 16.f / 9.f, .1f, 1000.f);
        const glm::mat4 tree = glm::translate(glm::mat4(1.f), glm::vec3(0.f, 0.f, -50.f));
        const glm::mat4 ahead = glm::lookAt(glm::vec3(0.f), glm::vec3(0.f, 0.f, -1.f), glm::vec3(0.f, 1.f, 0.f));
        const glm::mat4 turned = glm::lookAt(glm::vec3(0.f), glm::vec3(-1.f, 0.f, -1.f), glm::vec3(0.f, 1.f, 0.f));
        const float a = computeScreenCoverage(tree, box, ahead, proj), b = computeScreenCoverage(tree, box, turned, proj);
        require(a > 0.f && std::abs(a - b) < 1e-5f * a, "screen coverage does not change when the camera turns");
    }
    std::vector<MeshLodLevel> thresholds{{nullptr, nullptr, .1f}, {nullptr, nullptr, 0.f}};
    require(selectLodIndex(.095f, thresholds, 0, .1f) == 0, "hysteresis preserves near level");
    require(selectLodIndex(.08f, thresholds, 0, .1f) == 1, "coverage selects far level");
    require(selectLodIndex(.105f, thresholds, 1, .1f) == 1, "hysteresis preserves far level");
    require(selectLodIndex(.12f, thresholds, 1, .1f) == 0, "coverage restores near level");
    require(LODGroupBehaviour::shownLevel(0, 1, 3) == 1, "a finest level holds a near group back");
    require(LODGroupBehaviour::shownLevel(2, 1, 3) == 2, "a finest level never makes a far group finer");
    require(LODGroupBehaviour::shownLevel(0, 0, 3) == 0, "no finest level lets coverage decide");
    require(LODGroupBehaviour::shownLevel(0, 5, 3) == 2, "a finest level past the chain shows its coarsest");
    group.setFinestLevel(-3);
    require(group.finestLevel() == 0, "a negative finest level means none");
    // Cross-fade: complementary shares, serialized, off by default.
    require(group.crossFade() == 0.f && group.fadingLevel() == -1, "cross-fade is off by default");
    require(LODGroupBehaviour::incomingFade(0.f) > 0.f && LODGroupBehaviour::incomingFade(0.f) < .01f,
            "an incoming level starts with almost no pixels");
    require(LODGroupBehaviour::outgoingFade(0.f) == 0.f, "an outgoing level starts with every pixel");
    require(LODGroupBehaviour::incomingFade(.3f) == .3f && LODGroupBehaviour::outgoingFade(.3f) == -.3f,
            "the two levels share the pixels");
    require(LODGroupBehaviour::incomingFade(1.f) == 1.f, "a finished fade draws every pixel");
    group.setCrossFade(-1.f);
    require(group.crossFade() == 0.f, "a negative cross-fade means none");
    {
        LODGroupBehaviour fading;
        fading.setLevels({{"Near", .1f}, {"Far", 0.f}});
        fading.setCrossFade(.5f);
        nlohmann::json saved;
        fading.save(saved);
        LODGroupBehaviour loaded;
        loaded.load(saved);
        require(loaded.crossFade() == .5f, "cross-fade survives serialization");
    }
}

void streamedGroupContract() {
    Scene scene;
    auto* root = scene.createChild<Node>("Building");
    auto* close = root->createChild<Node>("Near");
    auto* middle = root->createChild<Node>("Mid");
    auto* far = root->createChild<Node>("Far");
    auto* group = root->addBehaviour<LODGroupBehaviour>();
    group->setLevels({{"Near", .1f}, {"Mid", .01f}, {"Far", 0.f}});
    const Aabb bounds{glm::vec3(-1.f), glm::vec3(1.f)};
    group->setBounds(bounds);
    group->setCrossFade(.5f);
    scene.update(.01f);
    const auto projection = glm::perspective(glm::radians(60.f), 1.f, .1f, 1000.f);
    const glm::mat4 view(1.f);
    uint64_t frame = 0;
    auto update = [&](float distance, float dt = .1f) {
        root->transform().position = {0.f, 0.f, -distance};
        scene.update(0.f);
        Time::advance(dt);
        group->updateForView(view, projection, ++frame);
        scene.refreshHierarchy();
    };
    for (int level = 0; level < 3; ++level) group->setLevelReady(level, false);
    require(group->desiredLevel() == -1, "a streamed group has no desired level before its first view");
    update(5.f);
    require(group->desiredLevel() == 0 && group->activeLevel() == -1,
            "explicit bounds select Near while every child is empty and unavailable");
    require(!close->visible() && !middle->visible() && !far->visible(),
            "unavailable representations stay hidden");
    group->setFinestLevel(1);
    group->setLevelReady(2, true);
    update(5.f);
    require(group->desiredLevel() == 1 && group->activeLevel() == 2 && far->visible(),
            "Far provides a cold initial fallback within the finest-level budget");
    group->setFinestLevel(0);
    update(5.f);
    require(group->desiredLevel() == 0 && group->activeLevel() == 2,
            "releasing a cold finest-level budget requests Near while keeping ready Far");
    group->setLevelReady(1, true);
    update(5.f);
    require(group->activeLevel() == 2 && group->fadingLevel() == -1,
            "a ready Mid does not replace the existing Far while desired Near is unavailable");
    group->setLevelReady(0, true);
    update(5.f);
    require(group->activeLevel() == 0 && group->fadingLevel() == 2 && close->visible() && far->visible(),
            "the requested upload cross-fades from its available fallback");
    update(100.f, .025f);
    require(group->activeLevel() == 2 && group->fadingLevel() == 0 && close->visible() && far->visible(),
            "a streamed fade reverses when coverage returns to the outgoing level");
    update(5.f, .025f);
    require(group->activeLevel() == 0 && group->fadingLevel() == 2,
            "a streamed fade can reverse again without losing either representation");
    group->setLevelReady(0, false);
    update(5.f);
    require(group->activeLevel() == 1 && group->fadingLevel() == -1 && middle->visible() && !far->visible(),
            "an unavailable incoming level cancels its fade and restores a complete ready fallback");
    group->setLevelReady(0, true);
    update(5.f);
    require(group->activeLevel() == 0 && group->fadingLevel() == 1,
            "a reloaded requested level cross-fades from the fallback");
    update(5.f, .5f);
    require(group->activeLevel() == 0 && group->fadingLevel() == -1 && close->visible() && !middle->visible(),
            "a completed streamed fade exposes only the requested representation");
    update(100.f);
    require(group->fadingLevel() == 0, "a streamed coarser transition retains the outgoing Near");
    group->updateForView(view, projection, 0);
    require(group->fadingLevel() == -1 && far->visible() && !close->visible(),
            "a camera cut completes an existing streamed fade immediately");
    update(5.f);
    group->setCrossFade(0.f);
    group->setLevelReady(1, false);
    update(50.f);
    require(group->desiredLevel() == 1 && group->activeLevel() == 0 && close->visible(),
            "a coarser upload pending retains the previous available representation");
    group->setFinestLevel(1);
    update(50.f);
    require(group->desiredLevel() == 1 && group->activeLevel() == 2 && !close->visible() && far->visible(),
            "a finest-level budget rejects a finer fallback");
    group->setLevelReady(2, false);
    update(50.f);
    require(group->activeLevel() == -1 && !close->visible(),
            "no allowed fallback never exposes a level finer than the budget");
    group->setLevelReady(1, true);
    update(50.f);
    require(group->activeLevel() == 1 && middle->visible(), "a completed coarser upload becomes visible");
    group->setFinestLevel(0);
    update(5.f);
    require(group->desiredLevel() == 0 && group->activeLevel() == 0,
            "releasing the finest-level budget immediately returns to coverage selection");
    group->setCrossFade(.3f);
    group->setFinestLevel(1);
    update(5.f);
    require(group->activeLevel() == 1 && group->fadingLevel() == 0 && close->visible(),
            "a stricter finest-level budget fades out the loaded previous representation");
    update(5.f, .5f);
    require(group->fadingLevel() == -1 && !close->visible() && middle->visible(),
            "the finer outgoing representation releases visibility after its budget transition");
    group->setFinestLevel(0);
    group->setCrossFade(0.f);
    // For this unit box and projection, coverage is 36 / distance squared.
    update(std::sqrt(36.f / .095f));
    require(group->desiredLevel() == 0, "streaming availability preserves Near coverage hysteresis");
    update(std::sqrt(36.f / .08f));
    require(group->desiredLevel() == 1, "coverage below the hysteresis band requests Mid");
    update(std::sqrt(36.f / .105f));
    require(group->desiredLevel() == 1, "streaming availability preserves Mid coverage hysteresis");
    update(std::sqrt(36.f / .12f));
    require(group->desiredLevel() == 0, "coverage above the hysteresis band requests Near");
    root->transform().scale = glm::vec3(10.f);
    update(100.f);
    require(group->desiredLevel() == 0, "explicit local bounds follow the group's world scale");
    group->setBounds({glm::vec3(-.1f), glm::vec3(.1f)});
    update(100.f);
    require(group->desiredLevel() == 2, "replacing explicit bounds changes coverage without loaded Near geometry");
    bool refused = false;
    try { group->setBounds({glm::vec3(1.f), glm::vec3(-1.f)}); }
    catch (const std::invalid_argument&) { refused = true; }
    require(refused, "inverted explicit LOD bounds are refused");
    refused = false;
    try { group->setBounds({glm::vec3(0.f), glm::vec3(std::numeric_limits<float>::infinity())}); }
    catch (const std::invalid_argument&) { refused = true; }
    require(refused, "non-finite explicit LOD bounds are refused");
    refused = false;
    try { group->setLevelReady(3, true); }
    catch (const std::out_of_range&) { refused = true; }
    require(refused, "readiness outside the configured chain is refused");
    refused = false;
    try { group->setLevelReady(-1, true); }
    catch (const std::out_of_range&) { refused = true; }
    require(refused, "negative readiness levels are refused");
    nlohmann::json saved;
    group->save(saved);
    require(saved.size() == 1 && saved.contains("levels"), "streaming bounds and readiness remain runtime-only");
    group->setLevels({{"Near", .1f}, {"Mid", .01f}, {"Far", 0.f}});
    root->transform().scale = glm::vec3(1.f);
    group->setBounds(bounds);
    update(5.f);
    require(group->activeLevel() == 0, "reconfiguring levels restores default ready availability");
    group->setEnabled(false);
    require(close->visible() && middle->visible() && far->visible(),
            "disabling a streamed group restores child visibility");
    Time::advance(0.f);
}
// Free space in pieces is packed when an upload needs it whole, and what was
// resident is still there, byte for byte, where its owner now finds it.
void gpuCompaction(VulkanDevice& device) {
    GeometryRegistry arena(device, 64, 96);
    auto mesh = [](uint32_t marker) {
        std::vector<Vertex> vertices(16);
        for (uint32_t i = 0; i < vertices.size(); ++i) vertices[i].pos = glm::vec3(float(marker), float(i), 0.f);
        std::vector<uint32_t> indices(24);
        for (uint32_t i = 0; i < indices.size(); ++i) indices[i] = marker * 100 + i;
        return std::make_pair(vertices, indices);
    };
    GeometryAllocation a, b, c, d;
    uint32_t marker = 1;
    for (GeometryAllocation* alloc : {&a, &b, &c, &d}) {
        auto [vertices, indices] = mesh(marker++);
        arena.allocate(*alloc, vertices, indices);
    }
    arena.free(a);
    arena.free(c);
    require(arena.usage().largestFreeIndices == 24 && arena.compactions() == 0, "free space in two pieces");
    auto [vertices, indices] = mesh(9);
    vertices.resize(32);
    indices.resize(48);
    GeometryAllocation big;
    arena.allocate(big, vertices, indices);
    require(arena.compactions() == 1 && big.indexCount == 48, "a fragmented arena is packed for an upload that fits in total");
    require(b.firstIndex == 0 && d.firstIndex == 24 && b.vertexOffset == 0 && d.vertexOffset == 16,
            "resident meshes are packed in order and their owners see where");

    // Read the moved geometry back.
    Buffer readIndices(device, 96 * sizeof(uint32_t), rhi::BufferUsage::TransferDst, MemoryUsage::HostVisible);
    Buffer readVertices(device, 64 * sizeof(Vertex), rhi::BufferUsage::TransferDst, MemoryUsage::HostVisible);
    device.withSingleTimeEncoder([&](rhi::CommandEncoder& enc) {
        enc.copyBufferToBuffer(*arena.indexBuffer(), readIndices, readIndices.size());
        enc.copyBufferToBuffer(*arena.vertexBuffer(), readVertices, readVertices.size());
    });
    const auto* index = static_cast<const uint32_t*>(readIndices.mapped());
    const auto* vertex = static_cast<const Vertex*>(readVertices.mapped());
    require(index && vertex, "readback buffers are mapped");
    require(index[b.firstIndex] == 200 && index[b.firstIndex + 23] == 223 &&
                index[d.firstIndex] == 400 && index[d.firstIndex + 23] == 423,
            "indices moved intact");
    require(vertex[b.vertexOffset].pos.x == 2.f && vertex[d.vertexOffset + 15].pos.y == 15.f &&
                vertex[d.vertexOffset].pos.x == 4.f,
            "vertices moved intact");
    arena.free(b);
    arena.free(d);
    arena.free(big);
    require(arena.usage().indices == 0 && arena.usage().allocations == 0, "everything freed");
}

void gpuCollisionFrames(ResourceManager& resources) {
    std::vector<Vertex> vertices(4);
    const glm::vec3 points[]={{-196.25f,12.125f,-172.123f},{-179.25f,12.125f,-172.123f},
                              {-179.25f,21.125f,-172.123f},{-196.25f,21.125f,-172.123f}};
    for(int i=0;i<4;++i){vertices[i].pos=points[i];vertices[i].normal={0,0,1};}
    auto* mesh=resources.getMesh(resources.registerMemoryMesh(vertices,{0,1,2,0,2,3}));
    Scene scene;
    auto* frame=scene.createChild<Node>("Distant frame");
    frame->transform().position={-199832.390625f,-2076780.5f,-4689437.5f};
    frame->transform().rotation=glm::angleAxis(.833f,glm::vec3(1,0,0))*glm::angleAxis(.0464f,glm::vec3(0,0,1));
    auto* body=frame->createChild<StaticBodyNode>();
    auto* shape=body->createChild<CollisionShapeNode>();shape->shapeType=CollisionShapeType::Mesh;
    auto* branch=body->createChild<Node>();branch->transform().position={.34f,.12f,.25f};
    branch->transform().scale={1.2f,.9f,1.1f};
    auto* drawn=branch->createChild<MeshNode>("Wall",mesh,nullptr);
    scene.update(1.f/60.f); // Build while its parent is millions of metres away.
    const glm::vec3 destination{.01465f,-413.545f,-210.752f};
    scene.rebaseSubtreeTo(*frame,destination,glm::angleAxis(.012f,glm::vec3(0,1,0)));
    near(glm::vec3(frame->worldTransform()[3]),destination,"distant mesh frame reaches the explicit destination");
    scene.update(1.f/60.f);
    const glm::vec3 probe(drawn->worldTransform()*glm::vec4(-187.25f,16.125f,-172.123f,1));
    const glm::vec3 normal=glm::normalize(glm::mat3(drawn->worldTransform())*glm::vec3(0,0,1));
    for(float side:{-1.f,1.f}) {
        auto hit=scene.physics()->raycast(probe+normal*side,-normal*side,2.f);
        require(hit.hit&&hit.body==body->bodyId()&&std::abs(hit.distance-1.f)<.01f,"rebased mesh collider matches its drawn wall");
        const auto hits=scene.physics()->overlapSphere(probe+normal*side*.02f,.03f);
        require(std::find(hits.begin(),hits.end(),body->bodyId())!=hits.end(),"thin-wall overlap survives a distant initial frame");
    }
}

// Optional device proof: --gpu. The default CTest suite stays headless.
void gpuPipelineBatches(VulkanDevice& device) {
    rhi::BindGroupLayout inputs(device, std::vector<rhi::BindGroupLayoutEntry>{
        {0,rhi::BindingType::CombinedImageSampler,rhi::ShaderStages::Fragment}});
    Pipeline::Desc desc;
    desc.vertPath=shaderPath("tonemap.vert.spv");
    desc.fragPath=shaderPath("bloom_downsample.frag.spv");
    desc.colorFormats={rhi::Format::RGBA16Float};desc.bindGroupLayouts={&inputs};
    desc.vertexInput=false;desc.depthTest=false;desc.depthWrite=false;
    desc.cullMode=rhi::CullMode::None;desc.pushConstantSize=32;
    auto additive=desc;additive.blendMode=rhi::BlendMode::Additive;
    auto built=buildGraphicsPipelines(device,{desc,additive,desc});
    require(built.size()==3&&built[0]->layout()&&built[1]->layout()&&built[2]->layout(),
            "parallel pipeline batch publishes every variant in input order");
    auto invalid=desc;invalid.fragPath="missing-pipeline-test.frag.spv";
    for(const auto& jobs:{std::vector<Pipeline::Desc>{invalid,desc},std::vector<Pipeline::Desc>{desc,invalid}}) {
        bool failed=false;
        try{buildGraphicsPipelines(device,jobs);}catch(const std::runtime_error&){failed=true;}
        require(failed,"either pipeline lane joins and propagates a shader failure");
    }
    require(buildGraphicsPipelines(device,{}).empty(),"empty pipeline batch owns no work");
    require(buildGraphicsPipelines(device,{desc})[0]->layout(),"pipeline construction survives a failed batch");

    rhi::RenderTextureDesc hdr;hdr.format=rhi::Format::RGBA16Float;
    hdr.width=64;hdr.height=64;hdr.usage=rhi::TextureUsage::ColorAttachment|rhi::TextureUsage::Sampled;
    rhi::RenderTexture first(device,hdr);
    PostProcessor post(device,{64,64},hdr.format,first.view());
    hdr.width=128;hdr.height=96;rhi::RenderTexture second(device,hdr);
    auto& profiler=Profiler::instance();profiler.setEnabled(true);profiler.beginFrame();
    post.resize({128,96},second.view());
    post.resize({64,64},first.view());
    profiler.endFrame();profiler.setEnabled(false);
    bool compiled=false;
    for(const auto& counter:profiler.latestFrame().counters)
        if(std::string(counter.name)=="GPU/GraphicsPipelinesCreated"&&counter.value>0)compiled=true;
    require(!compiled&&post.bloomView(),"surface resize retains bloom pipelines and recreates valid targets");
}

void gpuQueuedMeshes(ResourceManager& resources) {
    MeshCache cache(resources.geometry());
    const auto before=resources.geometryUsage();
    std::vector<Vertex> vertices(4);
    vertices[0].pos={-1,0,0};vertices[1].pos={1,0,0};vertices[2].pos={1,2,0};vertices[3].pos={-1,2,0};
    const std::vector<uint32_t> indices{0,1,2,0,2,3};
    const auto data=prepareMesh({vertices,indices});
    AssetLoader loader;
    auto id=cache.queueMemory(data,{},nullptr);
    auto* proxy=cache.get(id,nullptr,loader,0);
    require(proxy&&!proxy->loaded()&&proxy->collisionVertices().empty(),"queued mesh has stable empty proxy");
    cache.pumpUploads(sizeof(Vertex),1000.);
    require(!proxy->loaded()&&proxy->geometryAllocation().indexCount==0&&proxy->collisionVertices().empty(),
            "partial GPU upload cannot draw or collide");
    require(cache.hasPendingLoads(),"partial upload is not settled");
    resources.geometry().compact();
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while(cache.hasPendingLoads()&&std::chrono::steady_clock::now()<deadline) {
        cache.pumpUploads(sizeof(Vertex),1000.);std::this_thread::yield();
    }
    require(proxy->loaded()&&!cache.hasPendingLoads(),"bounded slices eventually publish Ready");
    require(proxy->collisionIndices()==indices&&proxy->collisionVertices().size()==vertices.size(),"Ready publishes collision data");
    require(proxy->bounds().max.y==2.f,"worker-prepared bounds are published with geometry");
    require(cache.get(id,nullptr,loader,0)==proxy,"proxy address stays stable");
    Buffer read(resources.device(),indices.size()*sizeof(uint32_t),rhi::BufferUsage::TransferDst,MemoryUsage::HostVisible);
    resources.device().withSingleTimeEncoder([&](rhi::CommandEncoder& enc){
        enc.copyBufferToBuffer(*resources.geometry().indexBuffer(),read,read.size(),uint64_t(proxy->allocation().firstIndex)*sizeof(uint32_t),0);
    });
    require(std::memcmp(read.mapped(),indices.data(),read.size())==0,"all uploaded index slices survive compaction");
    const auto pending=cache.queueMemory(data,{},nullptr);
    cache.pumpUploads(sizeof(Vertex),1000.);
    require(!cache.get(pending,nullptr,loader,0)->loaded(),"second upload is partial");
    GpuGraveyard graveyard;
    cache.sweepUnused({proxy},graveyard,0);
    require(!cache.hasPendingLoads(),"evicted scene cancels its partial upload");
    require(resources.geometryUsage().vertices==before.vertices+vertices.size(),"cancellation releases reserved geometry");
    cache.clear();
    require(resources.geometryUsage().vertices==before.vertices,"streamed cache releases its allocations");
}

void gpuUploadLifetime(VulkanDevice& device) {
    auto retained=std::make_shared<int>(7);std::weak_ptr<int> weak=retained;
    const auto serial=device.submitUpload([](rhi::CommandEncoder&){},retained);
    retained.reset();
    require(!weak.expired(),"submitted uploads own temporary resources until completion is polled");
    device.waitIdle();
    require(device.uploadComplete(serial)&&weak.expired(),"completed fence releases command and temporary ownership");
    const auto pending=device.pendingUploads();
    bool failed=false;
    try {device.submitUpload([](rhi::CommandEncoder&){throw std::runtime_error("record failure");},{});}
    catch(const std::runtime_error&){failed=true;}
    require(failed&&device.pendingUploads()==pending,"failed recording does not leave an in-flight upload");

    std::vector<uint8_t> pixels(8*8*4);
    for(size_t i=0;i<pixels.size();i+=4){pixels[i]=32;pixels[i+1]=64;pixels[i+2]=128;pixels[i+3]=255;}
    Texture image(device,pixels.data(),8,8,rhi::Format::RGBA8Srgb,true,rhi::AddressMode::Repeat,true);
    require(image.mipLevels()==4,"asynchronous texture keeps the full mip chain");
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while(!image.uploadReady()&&std::chrono::steady_clock::now()<deadline)std::this_thread::yield();
    require(image.uploadReady(),"texture readiness follows its upload and mip fence");
    Buffer read(device,16,rhi::BufferUsage::TransferDst,MemoryUsage::HostVisible);
    device.withSingleTimeEncoder([&](rhi::CommandEncoder& enc){
        enc.transition(image.image(),rhi::ResourceState::ShaderRead,rhi::ResourceState::CopySrc);
        enc.copyTextureToBuffer(image.image(),read,2,2,2);
        enc.transition(image.image(),rhi::ResourceState::CopySrc,rhi::ResourceState::ShaderRead);
    });
    const auto* actual=static_cast<const uint8_t*>(read.mapped());
    for(size_t i=0;i<16;++i)require(std::abs(int(actual[i])-int(pixels[i%4]))<=1,
        "asynchronous mip generation preserves sampled texels");
    auto retired=std::make_shared<Texture>(device,pixels.data(),8,8,rhi::Format::RGBA8Srgb,true,rhi::AddressMode::Repeat,false);
    std::weak_ptr<Texture> weakTexture=retired;
    const auto retirement=device.submitUpload([](rhi::CommandEncoder&){},retired);
    retired.reset();device.waitIdle();
    require(device.uploadComplete(retirement)&&weakTexture.expired(),
        "upload retirement can release textures whose own upload fence already completed");
}

void gpuQueuedColliderPublication(ResourceManager& resources) {
    std::vector<Vertex> vertices(3);
    vertices[0].pos={-1,0,0};vertices[1].pos={1,0,0};vertices[2].pos={0,2,0};
    const auto id=resources.queueMemoryMesh(prepareMesh({vertices,{0,1,2}}));
    auto* mesh=resources.getMesh(id);
    Scene scene;
    auto* room=scene.createChild<Node>("Pending room");room->setEnabled(false);
    auto* body=room->createChild<StaticBodyNode>();
    auto* shape=body->createChild<CollisionShapeNode>();shape->shapeType=CollisionShapeType::Mesh;
    body->createChild<MeshNode>("Wall",mesh,nullptr);
    auto* furniture=room->createChild<StaticBodyNode>();
    auto* box=furniture->createChild<CollisionShapeNode>();
    box->shapeType=CollisionShapeType::Box;box->halfExtents=glm::vec3(.5f);
    scene.refreshHierarchy();
    require(scene.resourceUsage().meshes.count(mesh),"disabled pending room owns queued meshes");
    auto usage=scene.resourceUsage();usage.meshes.insert(resources.getMesh(kAssetBuiltinCube));
    resources.trimUnused(usage);
    scene.update(.01f);
    require(body->bodyId().IsInvalid()&&furniture->bodyId().IsInvalid(),
            "pending interior has no mesh or invisible furniture colliders");
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while(resources.assetLoadsSettled()==false&&std::chrono::steady_clock::now()<deadline) {
        resources.pumpAssetLoads();std::this_thread::yield();
    }
    require(mesh->loaded(),"owned interior queue settles while its root is disabled");
    scene.update(.01f);
    require(body->bodyId().IsInvalid()&&furniture->bodyId().IsInvalid(),
            "GPU completion alone does not activate a pending room");
    room->setEnabled(true);scene.update(.01f);
    require(!body->bodyId().IsInvalid()&&!furniture->bodyId().IsInvalid(),
            "ready room publishes mesh and furniture colliders together");
    room->setEnabled(false);scene.update(.01f);
    require(body->bodyId().IsInvalid()&&furniture->bodyId().IsInvalid(),
            "releasing the room removes both collider types");
    scene.clearChildren();
    resources.trimUnused({});
}

void gpuPreparedTextures(VulkanDevice& device) {
    ResourceManager resources(device,nullptr,{4096,16384});
    constexpr uint32_t width=1024,height=1024;
    std::vector<uint8_t> pixels(width*height*4);
    for(uint32_t y=0;y<height;++y)for(uint32_t x=0;x<width;++x) {
        const size_t i=(size_t(y)*width+x)*4;
        pixels[i]=uint8_t(y%256);pixels[i+1]=uint8_t(x%256);pixels[i+2]=96;pixels[i+3]=255;
    }
    const auto encoded=encodePngRGBA8(pixels.data(),width,height);
    const auto id=resources.queueMemoryTexture(encoded.data(),encoded.size(),false);
    require(!resources.assetLoadsSettled(),"texture preparations are part of settled readiness");
    unsigned pumps=0;
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while(!resources.assetLoadsSettled()&&std::chrono::steady_clock::now()<deadline) {
        resources.pumpAssetLoads();++pumps;std::this_thread::yield();
    }
    auto* texture=resources.getTexture(id,false);
    require(resources.assetLoadsSettled()&&texture&&texture!=resources.missingTexture(),"worker allocation and row uploads publish a texture");
    require(pumps>=4,"large texture transfers require multiple bounded pumps");
    Buffer read(device,pixels.size(),rhi::BufferUsage::TransferDst,MemoryUsage::HostVisible);
    device.withSingleTimeEncoder([&](rhi::CommandEncoder& enc){
        enc.transition(texture->image(),rhi::ResourceState::ShaderRead,rhi::ResourceState::CopySrc);
        enc.copyTextureToBuffer(texture->image(),read,width,height);
        enc.transition(texture->image(),rhi::ResourceState::CopySrc,rhi::ResourceState::ShaderRead);
    });
    require(std::memcmp(read.mapped(),pixels.data(),pixels.size())==0,"all row slices preserve the source pixels exactly");
    VkPhysicalDeviceProperties properties{};vkGetPhysicalDeviceProperties(device.physicalDevice(),&properties);
    const uint32_t rejectedWidth=properties.limits.maxImageDimension2D+1;
    std::vector<uint8_t> unsupported(size_t(rejectedWidth)*4,255);
    const auto badPNG=encodePngRGBA8(unsupported.data(),rejectedWidth,1);
    const auto bad=resources.queueMemoryTexture(badPNG.data(),badPNG.size(),false);
    const auto failedDeadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while(!resources.assetLoadsSettled()&&std::chrono::steady_clock::now()<failedDeadline) {
        resources.pumpAssetLoads();std::this_thread::yield();
    }
    require(resources.assetLoadsSettled()&&resources.getTexture(bad,false)==resources.missingTexture(),
        "allocation rejection settles to the preallocated missing texture without crashing");
    require(resources.getTexture(id,false)==texture,"failed texture preparation preserves other resident textures");

    AssetLoader loader;
    auto projection=loader.requestMemory(0x713,encoded,AssetPayloadKind::EnvironmentSH,environmentSHDecoder());
    const auto projectionDeadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while(!projection.ready()&&!projection.failed()&&std::chrono::steady_clock::now()<projectionDeadline)
        std::this_thread::yield();
    require(projection.ready()&&projection.payload(),"environment projection decodes on the asset worker");
    auto sh=std::static_pointer_cast<EnvironmentSH>(projection.payload());
    require(sh->coefficients[0].z>0.f&&std::isfinite(sh->coefficients[0].z),"worker environment projection retains nonzero finite irradiance");
}

void gpuAsyncGltf(VulkanDevice& device) {
    AssetRegistry registry;
    const auto root=std::filesystem::path(__FILE__).parent_path()/"fixtures";
    require(registry.load(root.string()),"async glTF fixture registry");
    ResourceManager resources(device,&registry,{4096,16384});
    auto handle=GLTFLoader::request("animation/two_bone.gltf",resources);
    require(bool(handle),"glTF request returns an async handle");
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
    while(!handle.ready()&&!handle.failed()&&std::chrono::steady_clock::now()<deadline)std::this_thread::yield();
    require(handle.ready(),"worker decodes glTF mesh, rig and animation fixture");
    Node animationRoot;
    require(GLTFLoader::instantiate(handle,animationRoot,resources)&&!handle,"worker-prepared animation hierarchy instantiates");
    require(!animationRoot.children().empty(),"animation-only glTF preserves its skeleton hierarchy");
    handle=GLTFLoader::request("streaming/triangle.gltf",resources);
    while(!handle.ready()&&!handle.failed()&&std::chrono::steady_clock::now()<deadline)std::this_thread::yield();
    require(handle.ready(),"worker decodes textured triangle glTF fixture");
    auto secondHandle=GLTFLoader::request("streaming/triangle.gltf",resources);
    Node rootNode;
    require(GLTFLoader::instantiate(handle,rootNode,resources)&&!handle,"main thread consumes prepared glTF");
    require(!rootNode.children().empty(),"prepared glTF preserves hierarchy");
    auto meshCount=0;
    rootNode.traverse([&](Node& node,const glm::mat4&){if(node.mesh()){++meshCount;require(!node.mesh()->loaded(),"glTF GPU work is queued");}});
    require(meshCount>0&&!resources.assetLoadsSettled(),"CPU readiness does not settle GPU queues");
    Node secondRoot;
    require(GLTFLoader::instantiate(secondHandle,secondRoot,resources),"deduplicated CPU payload supports two consumers");
    Mesh* firstMesh=nullptr;Mesh* secondMesh=nullptr;
    rootNode.traverse([&](Node& node,const glm::mat4&){if(node.mesh())firstMesh=node.mesh();});
    secondRoot.traverse([&](Node& node,const glm::mat4&){if(node.mesh())secondMesh=node.mesh();});
    require(firstMesh==secondMesh,"glTF instances keep stable shared GPU sub-assets");
    while(!resources.assetLoadsSettled()&&std::chrono::steady_clock::now()<deadline) { resources.pumpAssetLoads(); std::this_thread::yield(); }
    require(resources.assetLoadsSettled(),"async glTF GPU queues settle");
    rootNode.traverse([&](Node& node,const glm::mat4&){if(node.mesh())require(node.mesh()->loaded(),"async glTF mesh is drawable");});
    auto invalid=GLTFLoader::request("missing.gltf",resources);
    while(!invalid.failed()&&std::chrono::steady_clock::now()<deadline)std::this_thread::yield();
    require(invalid.failed()&&!GLTFLoader::instantiate(invalid,rootNode,resources),"missing glTF fails explicitly");
    auto specular=GLTFLoader::request("streaming/specular.gltf",resources);
    const auto specularDeadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
    while(!specular.ready()&&!specular.failed()&&std::chrono::steady_clock::now()<specularDeadline)
        std::this_thread::yield();
    Scene specularScene;
    auto* specularRoot=specularScene.createChild<Node>();
    require(GLTFLoader::instantiate(specular,*specularRoot,resources),"specular glTF instantiates asynchronously");
    std::vector<Material*> importedMaterials;
    specularRoot->traverse([&](Node& node,const glm::mat4&){if(node.material())importedMaterials.push_back(node.material());});
    require(importedMaterials.size()==3,"specular fixture retains all three material cases");
    const auto& core=importedMaterials[0]->desc();
    require(core.dielectricF0==glm::vec3(.04f)&&core.specularStrength==1.f
        &&core.specularColorId==kAssetInvalid&&core.specularStrengthId==kAssetInvalid,
        "core glTF keeps default reflectance without extra textures");
    const auto& authored=importedMaterials[1]->desc();
    require(glm::length(authored.dielectricF0-glm::vec3(.05f,.1f,.15f))<.00001f
        &&authored.specularStrength==.3f,"IOR and color factor determine unclamped dielectric reflectance");
    require(authored.specularColorId!=kAssetInvalid&&authored.specularStrengthId!=kAssetInvalid,
        "specular RGB and alpha textures are imported");
    const auto& disabled=importedMaterials[2]->desc();
    require(disabled.dielectricF0==glm::vec3(1.f)&&disabled.specularStrength==0.f,
        "zero IOR conversion and zero lobe strength remain explicit");
    auto changed=authored;changed.specularStrength=.7f;
    require(resources.getMaterial(changed)!=importedMaterials[1],"different specular factors do not alias material cache entries");
    specularRoot->setEnabled(false);specularScene.refreshHierarchy();
    require(specularScene.resourceUsage().textures.count(authored.specularColorId)
        &&specularScene.resourceUsage().textures.count(authored.specularStrengthId),
        "hidden specular materials retain both texture assets");
    while(!resources.assetLoadsSettled()&&std::chrono::steady_clock::now()<specularDeadline) {
        resources.pumpAssetLoads();std::this_thread::yield();
    }
    require(resources.assetLoadsSettled(),"specular textures settle and rebind");
    require(resources.getTexture(authored.specularColorId,true)!=resources.getTexture(authored.specularStrengthId,false),
        "color RGB uses sRGB while specular alpha uses a distinct linear texture");
    auto billboard=GLTFLoader::request("streaming/billboard.gltf",resources);
    const auto billboardDeadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
    while(!billboard.ready()&&!billboard.failed()&&std::chrono::steady_clock::now()<billboardDeadline)
        std::this_thread::yield();
    Node billboardRoot;
    require(GLTFLoader::instantiate(billboard,billboardRoot,resources),"billboard atlas glTF instantiates asynchronously");
    int billboards=0;
    billboardRoot.traverse([&](Node& node,const glm::mat4&){if(auto* mesh=dynamic_cast<MeshNode*>(&node)) {
        const auto& desc=mesh->material()->desc();
        require(desc.billboardColumns==8&&desc.billboardRows==5,"atlas dimensions survive glTF import");
        require(!mesh->castShadows(),"billboard quad is not a solid shadow caster");
        auto single=desc;single.billboardColumns=1;single.billboardRows=1;
        require(resources.getMaterial(single)!=mesh->material(),"different billboard atlases have distinct cached materials");
        ++billboards;
    }});
    require(billboards==1,"billboard fixture has one shared drawable");
    device.waitIdle();
}

void gpuStreamedGroupContract(ResourceManager& resources, Mesh* loaded) {
    Mesh pending(resources.geometry());
    Scene scene;
    auto* root = scene.createChild<Node>("StreamedBuilding");
    auto* close = root->createChild<MeshNode>("Near", &pending, nullptr);
    auto* far = root->createChild<MeshNode>("Far", loaded, nullptr);
    auto* group = root->addBehaviour<LODGroupBehaviour>();
    group->setLevels({{"Near", .1f}, {"Far", 0.f}});
    group->setBounds({glm::vec3(-1.f), glm::vec3(1.f)});
    group->setCrossFade(.5f);
    const auto projection = glm::perspective(glm::radians(60.f), 1.f, .1f, 1000.f);
    const auto view = glm::lookAt(glm::vec3(0, 0, 5), glm::vec3(0), glm::vec3(0, 1, 0));
    scene.update(0.f);
    Time::advance(.05f);
    group->updateForView(view, projection, 1);
    require(group->desiredLevel() == 0 && group->activeLevel() == 1 && far->visible() && !close->visible(),
            "an unloaded Near mesh falls back to loaded Far despite default ready availability");
    std::vector<Vertex> vertices(3);
    vertices[0].pos = {-1, 0, 0}; vertices[1].pos = {1, 0, 0}; vertices[2].pos = {0, 1, 0};
    pending.upload(vertices, {0, 1, 2});
    group->updateForView(view, projection, 2);
    require(group->activeLevel() == 0 && group->fadingLevel() == 1 && close->visible() && far->visible(),
            "a mesh upload completes readiness without a hierarchy mutation");
    require(std::abs(close->lodFade() - .1f) < 1e-6f && std::abs(far->lodFade() + .1f) < 1e-6f,
            "streamed mesh transitions use complementary incoming and outgoing fades");
    group->setLevelReady(0, false);
    group->updateForView(view, projection, 3);
    require(group->activeLevel() == 1 && group->fadingLevel() == -1 && far->lodFade() == 1.f,
            "losing incoming readiness restores all Far pixels");
    close->setMesh(nullptr);
    scene.refreshHierarchy();
    require(!scene.resourceUsage().meshes.count(&pending) && scene.resourceUsage().meshes.count(loaded),
            "removing a streamed inactive mesh releases its scene ownership while retaining the fallback");
    Time::advance(0.f);
}

void gpuLodGroups() {
    Window window(64, 64, "Streaming contracts", false);
    VulkanDevice device(window);
    ResourceManager resources(device, nullptr, {512, 1024});
    require(resources.geometryCapacity().vertices == 512 && resources.geometryCapacity().indices == 1024,
            "resource manager exposes configured geometry capacity");
    const GeometryUsage before = resources.geometryUsage();
    Mesh* mesh = resources.getMesh(kAssetBuiltinCube);
    require(mesh && mesh->loaded(), "mesh uploaded into configured arena");
    const GeometryUsage after = resources.geometryUsage();
    require(after.indices > before.indices && after.vertices > before.vertices &&
                after.allocations == before.allocations + 1,
            "the arena reports what an upload took");
    require(after.largestFreeIndices <= 1024 - after.indices, "the largest free range is free space");
    bool told = false;
    try {
        std::vector<Vertex> vertices(4);
        std::vector<uint32_t> indices(4096, 0);
        GeometryAllocation tooBig;
        resources.geometry().allocate(tooBig, vertices, indices);
    } catch (const std::runtime_error& e) {
        told = std::string(e.what()).find("4096 indices requested") != std::string::npos;
    }
    require(told, "an upload the arena cannot hold says what it asked and what was left");
    gpuCompaction(device);
    gpuPipelineBatches(device);
    gpuCollisionFrames(resources);
    gpuUploadLifetime(device);
    gpuQueuedMeshes(resources);
    gpuQueuedColliderPublication(resources);
    gpuAsyncGltf(device);
    gpuPreparedTextures(device);
    Scene scene;
    auto* root = scene.createChild<Node>();
    auto* terrain = root->createChild<TerrainRingsNode>();
    MaterialDesc terrainDesc;
    terrainDesc.baseColor = {0.2f, 0.18f, 0.15f, 1.0f};
    Material* terrainMaterial = resources.getMaterial(terrainDesc);
    TerrainRingsNode::Layer layer;
    layer.material = terrainMaterial;
    terrain->setLayer(0, layer);
    terrain->setVisible(false);
    scene.refreshHierarchy();
    require(scene.resourceUsage().materials.count(terrainMaterial), "hidden terrain retains its material");
    terrain->setLayer(0, {});
    scene.refreshHierarchy();
    require(!scene.resourceUsage().materials.count(terrainMaterial), "replaced terrain releases its material");
    auto* close = root->createChild<Node>("Near");
    close->createChild<MeshNode>("Left", mesh, nullptr)->transform().position.x = -1.f;
    close->createChild<MeshNode>("Right", mesh, nullptr)->transform().position.x = 1.f;
    auto* far = root->createChild<MeshNode>("Far", mesh, nullptr);
    auto* group = root->addBehaviour<LODGroupBehaviour>();
    group->setLevels({{"Near", .1f}, {"Far", 0.f}});
    scene.update(.01f);
    const auto projection = glm::perspective(glm::radians(60.f), 1.f, .1f, 1000.f);
    auto view = [](float distance) { return glm::lookAt(glm::vec3(0, 0, distance), glm::vec3(0), glm::vec3(0, 1, 0)); };
    scene.updateRenderLods(view(3), projection);
    require(group->activeLevel() == 0 && close->visible() && !far->visible() && scene.meshes().size() == 2,
            "near representation renders both primitives");
    scene.updateRenderLods(view(100), projection);
    require(group->activeLevel() == 1 && !close->visible() && far->visible() && scene.meshes().size() == 1,
            "far representation replaces all near primitives");
    require(scene.resourceUsage().meshes.count(mesh), "inactive LOD retains geometry");
    close->transform().scale = glm::vec3(100.f);
    scene.update(0.f);
    scene.updateRenderLods(view(100), projection);
    require(group->activeLevel() == 0, "changed child transforms invalidate group bounds");
    group->setEnabled(false);
    scene.refreshHierarchy();
    require(close->visible() && far->visible() && scene.meshes().size() == 3,
            "disabling a group releases child visibility");
    group->setEnabled(true);
    group->setLevels({});
    scene.refreshHierarchy();
    require(scene.meshes().size() == 3, "removing levels restores all representations");
    gpuStreamedGroupContract(resources, mesh);
    device.waitIdle();
}
}
int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    indexChanges(); ownershipAndReparenting(); transformAndCameraContract(); fixedStepsAndRebase(); rebaseDestinationPrecision(); rebaseCharacterAndJoint(); groupContract(); streamedGroupContract();
    if (argc > 1 && std::string(argv[1]) == "--gpu") gpuLodGroups();
    std::printf("[streaming] PASS (%d checks)\n", checks);
}
