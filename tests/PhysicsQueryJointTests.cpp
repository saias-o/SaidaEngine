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
#include "physics/CharacterBodyNode.hpp"
#include "physics/CollisionShapeNode.hpp"
#include "physics/JointNodes.hpp"
#include "physics/PhysicsWorld.hpp"
#include "physics/RigidBodyNode.hpp"
#include "physics/StaticBodyNode.hpp"
#include "graphics/Mesh.hpp"
#include <Jolt/Physics/Collision/Shape/MeshShape.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <limits>
#include <stdexcept>

using namespace saida;

namespace {

int gChecks = 0;

void require(bool condition, const char* what) {
    ++gChecks;
    if (!condition) {
        std::printf("[physics-query-joint] FAIL: %s\n", what);
        std::fflush(stdout);
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

    // A scene wall is an obstacle from either side. Box-only query tests
    // miss back-face rejection and roundoff at thin triangle surfaces.
    JPH::VertexList vertices{{-2,-2,0},{2,-2,0},{0,2,0}};
    JPH::IndexedTriangleList triangles{{0,1,2,0}};
    auto mesh=JPH::MeshShapeSettings(vertices,triangles).Create();
    require(mesh.IsValid(), "query wall mesh built");
    BodyDesc desc;desc.shape=mesh.Get().GetPtr();desc.position={10,3,0};desc.motion=BodyMotion::Static;
    const auto plane=world->createBody(desc);
    require(!plane.IsInvalid(), "query wall body built");
    for(float side:{1.f,-1.f}) {
        found=world->overlapSphere({10,3,side*.1f},.2f);
        require(std::find(found.begin(),found.end(),plane)!=found.end(), "sphere overlaps wall from either side");
        hit=world->raycast({10,3,side},{0,0,-side},2.f);
        require(hit.hit&&hit.body==plane&&std::abs(hit.distance-1.f)<.01f, "ray hits wall from either side");
    }
    QueryFilter ignorePlane;ignorePlane.ignore=plane;
    require(world->overlapSphere({10,3,-.1f},.2f,ignorePlane).empty(), "two-sided overlap keeps ignore filter");
    world->removeBody(plane);

    std::printf("[physics-query-joint] queries ok\n");
}

std::shared_ptr<const MeshCollisionData> wallCollisionData(bool portal = true) {
    auto data = std::make_shared<MeshCollisionData>();
    const auto quad = [&](float x0, float x1, float y0, float y1) {
        const auto base = uint32_t(data->positions.size());
        data->positions.insert(data->positions.end(), {{x0, y0, 0}, {x1, y0, 0},
                                                      {x1, y1, 0}, {x0, y1, 0}});
        data->indices.insert(data->indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
    };
    if (portal) {
        quad(-3.f, -.5f, 0.f, 3.f);
        quad(.5f, 3.f, 0.f, 3.f);
        quad(-.5f, .5f, 2.f, 3.f);
    } else quad(-3.f, 3.f, 0.f, 3.f);
    return data;
}

void testProceduralTriangleQueries() {
    Scene scene;
    auto* wall = scene.createChild<StaticBodyNode>();
    auto* shape = wall->createChild<CollisionShapeNode>();
    auto geometry = wallCollisionData();
    const std::weak_ptr<const MeshCollisionData> retained = geometry;
    shape->setTriangleMeshData(geometry);
    geometry.reset();
    require(!retained.expired(), "the shape retains immutable procedural collision data without a GPU mesh");
    step(scene, 1);
    auto* world = scene.physics();
    require(world && !wall->bodyId().IsInvalid() && world->bodyCount() == 1,
            "procedural triangle geometry builds one static body without rendered meshes");
    for (const float side : {1.f, -1.f}) {
        auto hit = world->raycast({2, 1, side}, {0, 0, -side}, 2.f);
        require(hit.hit && hit.body == wall->bodyId() && std::abs(hit.distance - 1.f) < .002f,
                "procedural wall rays hit the exact triangle plane from either side");
        require(!world->raycast({0, 1, side}, {0, 0, -side}, 2.f).hit,
                "a portal remains open to rays from either side");
        hit = world->raycast({0, 2.5f, side}, {0, 0, -side}, 2.f);
        require(hit.hit && hit.body == wall->bodyId(), "the lintel above the portal remains solid");
        const auto overlaps = world->overlapSphere({2, 1, side * .1f}, .2f);
        require(std::find(overlaps.begin(), overlaps.end(), wall->bodyId()) != overlaps.end(),
                "procedural triangle walls support two-sided occupancy queries");
    }
    require(world->overlapSphere({0, 1, 0}, .2f).empty(), "occupancy inside the portal stays clear");
    wall->transform().scale = {2.f, 3.f, .5f};
    step(scene, 1);
    require(!world->raycast({.75f, 1, 1}, {0, 0, -1}, 2.f).hit,
            "changing nonuniform body scale expands the existing portal");
    require(world->raycast({1.5f, 1, 1}, {0, 0, -1}, 2.f).hit,
            "a scaled portal keeps its neighbouring wall solid");
    require(world->raycast({0, 6.5f, 1}, {0, 0, -1}, 2.f).hit,
            "nonuniform scaling moves the procedural lintel to its scaled height");
    shape->setTriangleMeshData(wallCollisionData(false));
    step(scene, 1);
    require(retained.expired(), "replacing procedural data releases the previous CPU geometry");
    require(world->raycast({0, 1, 1}, {0, 0, -1}, 2.f).hit && world->bodyCount() == 1,
            "replacing data rebuilds the live triangles and closes the portal");
    shape->setTriangleMeshData(nullptr);
    step(scene, 1);
    require(!world->raycast({1.5f, 1, 1}, {0, 0, -1}, 2.f).hit,
            "clearing explicit geometry removes its old triangles from the live body");
    require(world->raycast({0, 0, 2}, {0, 0, -1}, 4.f).hit && world->bodyCount() == 1,
            "clearing explicit geometry restores mesh discovery and its documented box fallback");
    std::printf("[physics-query-joint] procedural triangle queries ok\n");
}

void testProceduralTriangleRebase() {
    Scene scene;
    auto* frame = scene.createChild<Node>();
    frame->transform().position = {-199832.390625f, -2076780.5f, -4689437.5f};
    frame->transform().rotation = glm::angleAxis(.833f, glm::vec3(1, 0, 0)) *
                                  glm::angleAxis(.0464f, glm::vec3(0, 0, 1));
    auto* wall = frame->createChild<StaticBodyNode>();
    wall->transform().position = {12, 222, 35};
    wall->transform().scale = {2, 3, .5f};
    auto* shape = wall->createChild<CollisionShapeNode>();
    shape->setTriangleMeshData(wallCollisionData());
    step(scene, 1);
    const auto id = wall->bodyId();
    require(!id.IsInvalid(), "procedural geometry builds in a distant rotated frame");
    const auto orientation = glm::angleAxis(.012f, glm::vec3(0, 1, 0));
    scene.rebaseSubtreeTo(*frame, {.01465f, -413.545f, -210.752f}, orientation);
    auto* world = scene.physics();
    const auto check = [&] {
        for (const float side : {1.f, -1.f}) {
            const auto origin = glm::vec3(wall->worldTransform() * glm::vec4(2, 1, side * 4.f, 1));
            const auto direction = orientation * glm::vec3(0, 0, -side);
            const auto hit = world->raycast(origin, direction, 4.f);
            require(hit.hit && hit.body == wall->bodyId() && std::abs(hit.distance - 2.f) < .002f,
                    "distant-frame rebasing retains exact local procedural wall geometry");
            const auto doorway = glm::vec3(wall->worldTransform() * glm::vec4(0, 1, side * 4.f, 1));
            require(!world->raycast(doorway, direction, 4.f).hit,
                    "distant-frame rebasing keeps the procedural doorway open");
        }
    };
    check();
    step(scene, 1);
    require(wall->bodyId() == id, "a rigid frame rebase preserves the procedural body identity after synchronization");
    check();
    std::printf("[physics-query-joint] procedural triangle rebase ok\n");
}

void testProceduralTriangleValidation() {
    Scene scene;
    auto* wall = scene.createChild<StaticBodyNode>();
    auto* shape = wall->createChild<CollisionShapeNode>();
    const auto good = wallCollisionData();
    shape->setTriangleMeshData(good);
    step(scene, 1);
    const auto id = wall->bodyId();
    const auto reject = [&](const std::shared_ptr<MeshCollisionData>& bad, const char* message) {
        bool refused = false;
        try { shape->setTriangleMeshData(bad); }
        catch (const std::invalid_argument&) { refused = true; }
        require(refused, message);
        step(scene, 1);
        const auto hit = scene.physics()->raycast({2, 1, 1}, {0, 0, -1}, 2.f);
        require(hit.hit && hit.body == id, "rejected procedural data preserves the existing shape and body");
    };
    auto bad = std::make_shared<MeshCollisionData>(*good);
    bad->positions.clear();
    reject(bad, "triangle data without positions is refused");
    bad = std::make_shared<MeshCollisionData>(*good);
    bad->indices.clear();
    reject(bad, "triangle data without indices is refused");
    bad = std::make_shared<MeshCollisionData>(*good);
    bad->indices.pop_back();
    reject(bad, "incomplete procedural triangles are refused");
    bad = std::make_shared<MeshCollisionData>(*good);
    bad->indices.front() = uint32_t(bad->positions.size());
    reject(bad, "a procedural triangle index outside its positions is refused");
    for (const float invalid : {std::numeric_limits<float>::infinity(),
                               std::numeric_limits<float>::quiet_NaN()}) {
        bad = std::make_shared<MeshCollisionData>(*good);
        bad->positions.front().x = invalid;
        reject(bad, "non-finite procedural positions are refused");
    }
    std::printf("[physics-query-joint] procedural triangle validation ok\n");
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

// "Do not hit myself" has to name every body the caster is made of. A
// CharacterBody is the caster that gets this wrong: it is a CharacterVirtual
// with a kinematic inner body and never fills bodyId_ at all, so a filter built
// from bodyId() alone ignores nothing and the forward probe every controller
// runs each frame answers with the controller's own capsule at distance 0 and a
// horizontal normal — indistinguishable from a wall, every frame.
void testCharacterIgnoresItsOwnBodies() {
    Scene scene;

    auto* floor = scene.createChild<StaticBodyNode>();
    floor->setName("Floor");
    addBoxShape(floor, {20.0f, 0.5f, 20.0f});

    auto* wall = scene.createChild<StaticBodyNode>();
    wall->setName("Wall");
    wall->transform().position = {4.0f, 1.5f, 0.0f};
    addBoxShape(wall, {0.5f, 1.5f, 2.0f});

    auto* character = scene.createChild<CharacterBodyNode>();
    character->setName("Player");
    character->transform().position = {0.0f, 1.5f, 0.0f};
    auto capsule = std::make_unique<CollisionShapeNode>();
    capsule->shapeType = CollisionShapeType::Capsule;
    capsule->radius = 0.4f;
    capsule->height = 1.6f;
    character->addChild(std::move(capsule));

    step(scene, 3);  // create the bodies and the character
    PhysicsWorld* world = scene.physics();
    require(world != nullptr, "physics world created");

    require(character->bodyId().IsInvalid(),
            "a character has no ordinary body id — this is why one id is not enough");
    require(!character->innerBodyId().IsInvalid(),
            "a character exposes its inner body once created");

    const glm::vec3 eye = character->transform().position;
    const glm::vec3 ahead{1.0f, 0.0f, 0.0f};

    // Unfiltered: the character is in the broadphase through its inner body, so
    // it answers its own ray first. That is correct and is what makes the
    // filter's job necessary.
    RaycastHit hit = world->raycast(eye, ahead, 10.0f);
    require(hit.hit && world->bodyUserData(hit.body) == character,
            "unfiltered, the character's own ray answers with the character");

    // The filter as it was built before: bodyId() only. Invalid for a
    // character, so it excludes nothing and the probe still reports itself.
    QueryFilter idOnly;
    idOnly.ignore = character->bodyId();
    hit = world->raycast(eye, ahead, 10.0f, idOnly);
    require(hit.hit && world->bodyUserData(hit.body) == character,
            "naming only bodyId() ignores nothing on a character");

    // Both bodies named: the ray leaves the character and finds the wall.
    QueryFilter both;
    both.ignore = character->bodyId();
    both.ignoreInner = character->innerBodyId();
    hit = world->raycast(eye, ahead, 10.0f, both);
    require(hit.hit, "the filtered ray still reaches the world");
    require(world->bodyUserData(hit.body) == wall,
            "the filtered ray reports the wall, not the caster");
    require(hit.distance > 3.0f,
            "the wall is reported at its real distance, not at zero");

    std::printf("[physics-query-joint] character self-exclusion ok\n");
}

} // namespace

int main() {
    testQueries();
    testProceduralTriangleQueries();
    testProceduralTriangleRebase();
    testProceduralTriangleValidation();
    testCharacterIgnoresItsOwnBodies();
    testFindByPath();
    testPointJointPendulum();
    testFixedJointWorldAndRebuild();
    testHingeJointPathReference();
    testBodyRemovalDropsConstraint();
    std::printf("[physics-query-joint] PASS (%d checks)\n", gChecks);
    return 0;
}
