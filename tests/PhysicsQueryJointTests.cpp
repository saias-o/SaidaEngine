// Headless proof of the V1 physics gameplay API (P0.4):
//   - scene queries: filtered raycast (sensor exclusion, ignored body) and
//     overlapSphere, straight against PhysicsWorld through a stepped Scene;
//   - node path resolution (Node::findByPath) used by joints to reference
//     bodies;
//   - joint nodes: PointJoint holds a pendulum, FixedJoint welds to the world,
//     HingeJoint keeps its body positionally pinned; a joint survives a body
//     rebuild (markDirty → new BodyID) and the removal of a referenced body
//     never leaves a dangling Jolt constraint.
#include "scene/Scene.hpp"
#include "physics/AreaNode.hpp"
#include "physics/CollisionShapeNode.hpp"
#include "physics/JointNodes.hpp"
#include "physics/PhysicsWorld.hpp"
#include "physics/RigidBodyNode.hpp"
#include "physics/StaticBodyNode.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>

using namespace saida;

namespace {

int gChecks = 0;

void require(bool condition, const char* what) {
    ++gChecks;
    if (!condition) {
        std::printf("[physics-query-joint] FAIL: %s\n", what);
        std::abort();
    }
}

void addBoxShape(Node* body, const glm::vec3& halfExtents) {
    auto cs = std::make_unique<CollisionShapeNode>();
    cs->shapeType = CollisionShapeType::Box;
    cs->halfExtents = halfExtents;
    body->addChild(std::move(cs));
}

void step(Scene& scene, int frames) {
    const float dt = 1.0f / 60.0f;
    for (int i = 0; i < frames; ++i) scene.update(dt);
}

float dist(const glm::vec3& a, const glm::vec3& b) { return glm::length(a - b); }

// ---- queries ---------------------------------------------------------------

void testQueries() {
    Scene scene;

    auto* floor = scene.createChild<StaticBodyNode>();
    floor->setName("Floor");
    addBoxShape(floor, {20.0f, 0.5f, 20.0f});  // top at y = 0.5

    auto* wall = scene.createChild<StaticBodyNode>();
    wall->setName("Wall");
    wall->transform().position = {5.0f, 1.0f, 0.0f};
    addBoxShape(wall, {0.5f, 1.0f, 2.0f});

    auto* trigger = scene.createChild<AreaNode>();
    trigger->setName("Trigger");
    trigger->transform().position = {0.0f, 1.5f, 0.0f};
    addBoxShape(trigger, {0.5f, 0.5f, 0.5f});

    step(scene, 3);  // create the bodies
    PhysicsWorld* world = scene.physics();
    require(world != nullptr, "physics world created");

    // Downward ray through the sensor: default filter skips it → floor hit.
    RaycastHit hit = world->raycast({0.0f, 3.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, 10.0f);
    require(hit.hit, "raycast hits the floor");
    require(world->bodyUserData(hit.body) == floor, "sensor skipped by default");
    require(std::fabs(hit.distance - 2.5f) < 0.05f, "hit distance ~2.5");
    require(std::fabs(hit.point.y - 0.5f) < 0.05f, "hit point on the floor top");
    require(hit.normal.y > 0.9f, "floor normal points up");

    // Same ray, sensors admitted → the Area is the closest hit.
    QueryFilter sensors;
    sensors.hitSensors = true;
    hit = world->raycast({0.0f, 3.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, 10.0f, sensors);
    require(hit.hit && world->bodyUserData(hit.body) == trigger,
            "hitSensors reports the Area first");

    // Horizontal ray to the wall; ignoring the wall makes the ray miss.
    hit = world->raycast({0.0f, 1.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, 10.0f);
    require(hit.hit && world->bodyUserData(hit.body) == wall, "raycast hits the wall");
    QueryFilter ignoreWall;
    ignoreWall.ignore = wall->bodyId();
    hit = world->raycast({0.0f, 1.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, 10.0f, ignoreWall);
    require(!hit.hit, "ignored body is transparent to the ray");

    // Overlap around the sensor: default → floor only; hitSensors → both.
    std::vector<JPH::BodyID> found = world->overlapSphere({0.0f, 1.0f, 0.0f}, 1.2f);
    require(found.size() == 1 && world->bodyUserData(found[0]) == floor,
            "overlap reports the floor and skips the sensor");
    found = world->overlapSphere({0.0f, 1.0f, 0.0f}, 1.2f, sensors);
    bool sawFloor = false;
    bool sawTrigger = false;
    for (JPH::BodyID id : found) {
        if (world->bodyUserData(id) == floor) sawFloor = true;
        if (world->bodyUserData(id) == trigger) sawTrigger = true;
    }
    require(found.size() == 2 && sawFloor && sawTrigger,
            "overlap with hitSensors reports floor and Area once each");

    // Far away → empty.
    require(world->overlapSphere({100.0f, 50.0f, 0.0f}, 1.0f).empty(),
            "empty overlap far from every body");

    std::printf("[physics-query-joint] queries ok\n");
}

// ---- node paths ------------------------------------------------------------

void testFindByPath() {
    Scene scene;
    auto* parent = scene.createChild<Node>("Parent");
    auto* childA = parent->createChild<Node>("A");
    auto* childB = parent->createChild<Node>("B");
    auto* leaf = childB->createChild<Node>("Leaf");

    require(childA->findByPath("../B") == childB, "sibling via ..");
    require(childA->findByPath("../B/Leaf") == leaf, "nested path");
    require(leaf->findByPath("../../A") == childA, "two levels up");
    require(childA->findByPath(".") == childA, "self via .");
    require(leaf->findByPath("/Parent/A") == childA, "absolute from root");
    require(childA->findByPath("../Missing") == nullptr, "missing segment fails");
    require(scene.findByPath("..") == nullptr, "above the root fails");

    std::printf("[physics-query-joint] findByPath ok\n");
}

// ---- joints ----------------------------------------------------------------

// A pendulum bob point-jointed to a world anchor 1 m above it must keep its
// distance to the pivot; the control bob (no joint) free-falls to the floor.
void testPointJointPendulum() {
    Scene scene;
    auto* floor = scene.createChild<StaticBodyNode>();
    addBoxShape(floor, {20.0f, 0.5f, 20.0f});

    auto* bob = scene.createChild<RigidBodyNode>();
    bob->setName("Bob");
    bob->transform().position = {0.0f, 5.0f, 0.0f};
    addBoxShape(bob, {0.25f, 0.25f, 0.25f});
    auto joint = std::make_unique<PointJointNode>();
    joint->transform().position = {0.0f, 1.0f, 0.0f};  // pivot at world (0,6,0)
    JointNode* jointPtr = joint.get();
    bob->addChild(std::move(joint));

    auto* control = scene.createChild<RigidBodyNode>();
    control->setName("Control");
    control->transform().position = {3.0f, 5.0f, 0.0f};
    addBoxShape(control, {0.25f, 0.25f, 0.25f});

    step(scene, 300);  // 5 s

    require(jointPtr->jointActive(), "point joint constraint is live");
    const glm::vec3 pivot{0.0f, 6.0f, 0.0f};
    const float radius = dist(bob->transform().position, pivot);
    require(std::fabs(radius - 1.0f) < 0.1f, "bob stays on the pendulum radius");
    require(bob->transform().position.y > 3.0f, "bob never fell to the floor");
    require(control->transform().position.y < 1.5f, "control bob fell freely");

    std::printf("[physics-query-joint] point joint ok (radius=%.3f control_y=%.3f)\n",
                radius, control->transform().position.y);
}

// A fixed joint with no body B welds its body to the world: the box must hover
// exactly where it spawned, and keep holding after its body is rebuilt.
void testFixedJointWorldAndRebuild() {
    Scene scene;
    auto* floor = scene.createChild<StaticBodyNode>();
    addBoxShape(floor, {20.0f, 0.5f, 20.0f});

    auto* box = scene.createChild<RigidBodyNode>();
    box->setName("Hover");
    box->transform().position = {0.0f, 4.0f, 0.0f};
    addBoxShape(box, {0.25f, 0.25f, 0.25f});
    auto joint = std::make_unique<FixedJointNode>();
    JointNode* jointPtr = joint.get();
    box->addChild(std::move(joint));

    step(scene, 180);
    require(jointPtr->jointActive(), "fixed joint constraint is live");
    require(std::fabs(box->transform().position.y - 4.0f) < 0.05f,
            "welded box hovers at its spawn height");

    // Rebuild the body (shape/param edit path): new BodyID → the joint must
    // recreate its constraint and keep holding.
    box->markDirty();
    step(scene, 180);
    require(jointPtr->jointActive(), "joint rebuilt after body recreation");
    require(std::fabs(box->transform().position.y - 4.0f) < 0.1f,
            "welded box still hovers after the rebuild");

    std::printf("[physics-query-joint] fixed joint + rebuild ok (y=%.3f)\n",
                box->transform().position.y);
}

// A hinge lets its body spin around the axis but pins it in translation: the
// door panel referenced through an explicit body path must not fall.
void testHingeJointPathReference() {
    Scene scene;
    auto* floor = scene.createChild<StaticBodyNode>();
    addBoxShape(floor, {20.0f, 0.5f, 20.0f});

    auto* frame = scene.createChild<StaticBodyNode>();
    frame->setName("Frame");
    frame->transform().position = {0.0f, 2.0f, 0.0f};
    addBoxShape(frame, {0.1f, 1.0f, 0.1f});

    auto* door = scene.createChild<RigidBodyNode>();
    door->setName("Door");
    door->transform().position = {0.6f, 2.0f, 0.0f};
    addBoxShape(door, {0.5f, 0.9f, 0.05f});

    // Joint authored under a plain group node: both bodies via explicit paths.
    auto* rig = scene.createChild<Node>("Rig");
    auto joint = std::make_unique<HingeJointNode>();
    joint->bodyA = "/Door";
    joint->bodyB = "/Frame";
    joint->axis = {0.0f, 1.0f, 0.0f};
    JointNode* jointPtr = joint.get();
    rig->addChild(std::move(joint));
    rig->transform().position = {0.0f, 2.0f, 0.0f};  // pivot on the frame axis

    step(scene, 300);
    require(jointPtr->jointActive(), "hinge joint constraint is live");
    require(door->transform().position.y > 1.5f, "hinged door does not fall");
    require(dist(door->transform().position, {0.0f, 2.0f, 0.0f}) < 1.0f,
            "door stays around its hinge pivot");

    std::printf("[physics-query-joint] hinge joint ok (door_y=%.3f)\n",
                door->transform().position.y);
}

// Removing a jointed body must drop the constraint (no dangling Jolt state)
// and further stepping must stay safe; the joint then reports inactive.
void testBodyRemovalDropsConstraint() {
    Scene scene;
    auto* floor = scene.createChild<StaticBodyNode>();
    addBoxShape(floor, {20.0f, 0.5f, 20.0f});

    auto* anchor = scene.createChild<StaticBodyNode>();
    anchor->setName("Anchor");
    anchor->transform().position = {0.0f, 6.0f, 0.0f};
    addBoxShape(anchor, {0.2f, 0.2f, 0.2f});

    auto* bob = scene.createChild<RigidBodyNode>();
    bob->setName("Bob2");
    bob->transform().position = {0.0f, 5.0f, 0.0f};
    addBoxShape(bob, {0.25f, 0.25f, 0.25f});

    auto* rig = scene.createChild<Node>("Rig2");
    auto joint = std::make_unique<PointJointNode>();
    joint->bodyA = "/Bob2";
    joint->bodyB = "/Anchor";
    JointNode* jointPtr = joint.get();
    rig->addChild(std::move(joint));
    rig->transform().position = {0.0f, 6.0f, 0.0f};

    step(scene, 120);
    require(jointPtr->jointActive(), "constraint live before removal");

    require(scene.removeChild(anchor), "anchor removed");  // destroys its body
    step(scene, 120);  // must not crash; joint detects the dead reference
    require(!jointPtr->jointActive(), "constraint dropped with its body");
    require(bob->transform().position.y < 2.0f, "freed bob fell");

    std::printf("[physics-query-joint] body removal ok\n");
}

} // namespace

int main() {
    testQueries();
    testFindByPath();
    testPointJointPendulum();
    testFixedJointWorldAndRebuild();
    testHingeJointPathReference();
    testBodyRemovalDropsConstraint();
    std::printf("[physics-query-joint] PASS (%d checks)\n", gChecks);
    return 0;
}
