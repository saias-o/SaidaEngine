#include "scene/Scene.hpp"
#include "core/Window.hpp"
#include "graphics/VulkanDevice.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include "authoring/SceneSnapshot.hpp"
#include "nodes/MeshNode.hpp"
#include "nodes/UIImageNode.hpp"
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
    std::vector<MeshLodLevel> thresholds{{nullptr, nullptr, .1f}, {nullptr, nullptr, 0.f}};
    require(selectLodIndex(.095f, thresholds, 0, .1f) == 0, "hysteresis preserves near level");
    require(selectLodIndex(.08f, thresholds, 0, .1f) == 1, "coverage selects far level");
    require(selectLodIndex(.105f, thresholds, 1, .1f) == 1, "hysteresis preserves far level");
    require(selectLodIndex(.12f, thresholds, 1, .1f) == 0, "coverage restores near level");
}
// Optional device proof: --gpu. The default CTest suite stays headless.
void gpuLodGroups() {
    Window window(64, 64, "Streaming contracts", false);
    VulkanDevice device(window);
    ResourceManager resources(device, nullptr, {512, 1024});
    require(resources.geometryCapacity().vertices == 512 && resources.geometryCapacity().indices == 1024,
            "resource manager exposes configured geometry capacity");
    Mesh* mesh = resources.getMesh(kAssetBuiltinCube);
    require(mesh && mesh->loaded(), "mesh uploaded into configured arena");
    Scene scene;
    auto* root = scene.createChild<Node>();
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
    device.waitIdle();
}
}
int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    indexChanges(); ownershipAndReparenting(); fixedStepsAndRebase(); rebaseCharacterAndJoint(); groupContract();
    if (argc > 1 && std::string(argv[1]) == "--gpu") gpuLodGroups();
    std::printf("[streaming] PASS (%d checks)\n", checks);
}
