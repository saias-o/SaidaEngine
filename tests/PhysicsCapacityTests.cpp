// Headless proof of the physics capacity contract:
//   - the body capacity is chosen at startup (PhysicsCapacity), reported back,
//     and an impossible one is rejected before any world is built;
//   - a body refused for capacity is said once, counted, and not asked for
//     again every frame: the refused node waits until the world has room;
//   - a removed body makes room, and a waiting node takes it;
//   - a compound of many shapes is one body, whatever it holds;
//   - a character whose inner body is refused is reported as refused.
#include "scene/Scene.hpp"
#include "core/Log.hpp"
#include "physics/CharacterBodyNode.hpp"
#include "physics/CollisionShapeNode.hpp"
#include "physics/PhysicsWorld.hpp"
#include "physics/StaticBodyNode.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using namespace saida;

namespace {

int gChecks = 0;

void require(bool condition, const char* what) {
    ++gChecks;
    if (!condition) {
        std::printf("[physics-capacity] FAIL: %s\n", what);
        std::fflush(stdout);
        std::abort();
    }
}

void step(Scene& scene, int frames) {
    for (int i = 0; i < frames; ++i) scene.update(1.0f / 60.0f);
}

StaticBodyNode* addBox(Scene& scene, const glm::vec3& at) {
    auto* body = scene.createChild<StaticBodyNode>();
    body->transform().position = at;
    auto shape = std::make_unique<CollisionShapeNode>();
    shape->shapeType = CollisionShapeType::Box;
    shape->halfExtents = glm::vec3(0.5f);
    body->addChild(std::move(shape));
    return body;
}

size_t refusalLines() {
    size_t lines = 0;
    for (const std::string& line : Log::recent(512))
        if (line.find("refused: all") != std::string::npos) ++lines;
    return lines;
}

void testCapacityIsConfigured() {
    require(PhysicsCapacity{}.bodies == 8192, "default body capacity is 8192");

    Scene scene;
    require(scene.setPhysicsCapacity({16, 64, 64}), "capacity set before the world exists");
    addBox(scene, glm::vec3(0.0f));
    step(scene, 1);
    require(scene.physics() != nullptr, "first body builds the world");
    require(scene.physics()->capacity().bodies == 16, "world reports the configured body capacity");
    require(scene.physics()->bodyCount() == 1, "world counts its bodies");
    require(!scene.setPhysicsCapacity({32, 64, 64}), "a world holding bodies keeps its capacity");
    require(scene.physics()->capacity().bodies == 16, "refused change leaves the capacity");

    bool threw = false;
    try {
        scene.setPhysicsCapacity({0, 64, 64});
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    require(threw, "zero bodies is rejected");
    threw = false;
    try {
        PhysicsWorld world({PhysicsCapacity::kMaxBodies + 1, 64, 64});
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    require(threw, "more bodies than a BodyID indexes is rejected");
}

void testRefusalSaidOnceNotRetried() {
    Scene scene;
    scene.setPhysicsCapacity({4, 64, 64});
    std::vector<StaticBodyNode*> bodies;
    for (int i = 0; i < 6; ++i) bodies.push_back(addBox(scene, glm::vec3(float(i) * 3.0f, 0.0f, 0.0f)));

    const size_t linesBefore = refusalLines();
    step(scene, 1);
    PhysicsWorld* world = scene.physics();
    require(world->bodyCount() == 4, "the world holds exactly its capacity");
    require(world->refusedBodies() == 2, "both bodies over capacity are refused");
    size_t refused = 0;
    for (auto* b : bodies) {
        if (b->bodyRefused()) {
            ++refused;
            require(b->bodyId().IsInvalid(), "a refused node has no body");
            require(b->physicsWorld() == nullptr, "a refused node lives in no world");
        }
    }
    require(refused == 2, "the refused nodes know they were refused");
    require(refusalLines() == linesBefore + 1, "the refusal is logged once, with its reason");

    step(scene, 30);
    require(world->refusedBodies() == 2, "a refused node is not retried while the world is full");
    require(refusalLines() == linesBefore + 1, "a full world says nothing more while nothing changes");

    // Removing a body makes room: one waiting node takes it, the other keeps
    // waiting without being refused again.
    StaticBodyNode* built = nullptr;
    for (auto* b : bodies) if (!b->bodyRefused()) { built = b; break; }
    scene.removeChild(built);
    step(scene, 1);
    require(world->bodyCount() == 4, "the freed slot is taken");
    require(world->refusedBodies() == 2, "the node still waiting is not refused again");
    refused = 0;
    for (const auto& child : scene.children())
        if (auto* b = dynamic_cast<StaticBodyNode*>(child.get())) refused += b->bodyRefused() ? 1 : 0;
    require(refused == 1, "one node is still waiting");

    // A new body over capacity after a removal is a new refusal: said again, once.
    addBox(scene, glm::vec3(0.0f, 0.0f, 9.0f));
    step(scene, 1);
    require(world->refusedBodies() == 3, "a new body over capacity is refused");
    require(refusalLines() == linesBefore + 2, "the refusal after a removal is said again, once");
    step(scene, 30);
    require(refusalLines() == linesBefore + 2, "and then nothing more");
}

void testCompoundIsOneBody() {
    Scene scene;
    scene.setPhysicsCapacity({4, 64, 64});
    auto* trunks = scene.createChild<StaticBodyNode>();
    for (int i = 0; i < 100; ++i) {
        auto shape = std::make_unique<CollisionShapeNode>();
        shape->shapeType = CollisionShapeType::Box;
        shape->halfExtents = glm::vec3(0.25f, 2.0f, 0.25f);
        shape->offset = glm::vec3(float(i % 10) * 4.0f, 2.0f, float(i / 10) * 4.0f);
        trunks->addChild(std::move(shape));
    }
    step(scene, 1);
    require(!trunks->bodyRefused() && !trunks->bodyId().IsInvalid(), "a compound of 100 boxes is built");
    require(scene.physics()->bodyCount() == 1, "a compound of 100 boxes is one body");
    const RaycastHit hit = scene.physics()->raycast(glm::vec3(36.0f, 10.0f, 36.0f), glm::vec3(0, -1, 0), 100.0f);
    require(hit.hit && hit.body == trunks->bodyId(), "the last box of the compound collides");
    require(std::abs(hit.point.y - 4.0f) < 1e-3f, "the box sits at its own offset");
}

void testCharacterInnerBodyRefused() {
    Scene scene;
    scene.setPhysicsCapacity({1, 64, 64});
    addBox(scene, glm::vec3(0.0f, -1.0f, 0.0f));
    auto* character = scene.createChild<CharacterBodyNode>();
    character->transform().position = glm::vec3(0.0f, 1.0f, 0.0f);
    auto capsule = std::make_unique<CollisionShapeNode>();
    capsule->shapeType = CollisionShapeType::Capsule;
    capsule->radius = 0.3f;
    capsule->height = 1.8f;
    capsule->axis = 1;
    character->addChild(std::move(capsule));
    step(scene, 1);
    require(character->bodyRefused(), "a character without room for its inner body is refused");
    require(character->innerBodyId().IsInvalid(), "the refused character has no inner body");
    require(scene.physics()->refusedBodies() == 1, "the inner body counts as a refusal");
    step(scene, 10);
    require(scene.physics()->refusedBodies() == 1, "the character is not rebuilt while the world is full");
}

} // namespace

int main() {
    testCapacityIsConfigured();
    testRefusalSaidOnceNotRetried();
    testCompoundIsOneBody();
    testCharacterInnerBodyRefused();
    std::printf("[physics-capacity] PASS (%d checks)\n", gChecks);
    return 0;
}
