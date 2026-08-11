// Headless proof of PhysicsWorld's impulse/force API, which the raycast vehicle
// drives its wheels with:
//   - a central impulse changes linear velocity by J/m and adds no spin;
//   - an off-centre impulse also spins the body, about r x J;
//   - pointVelocity reports linear velocity plus the spin contribution, which is
//     what a wheel contact patch actually moves at;
//   - a force is integrated over one fixed step and then cleared, so it must not
//     be mistaken for an impulse in per-frame code;
//   - a static body and an invalid id absorb all of it without moving or
//     crashing.
#include "physics/CollisionShapeNode.hpp"
#include "physics/PhysicsWorld.hpp"
#include "physics/RigidBodyNode.hpp"
#include "physics/StaticBodyNode.hpp"
#include "scene/Scene.hpp"

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
        std::printf("[physics-impulse] FAIL: %s\n", what);
        std::abort();
    }
}

void requireNear(float actual, float expected, float tolerance, const char* what) {
    ++gChecks;
    if (!(std::fabs(actual - expected) <= tolerance)) {
        std::printf("[physics-impulse] FAIL: %s (got %f, expected %f +/- %f)\n", what, actual,
                    expected, tolerance);
        std::abort();
    }
}

const float kDt = 1.0f / 60.0f;

void step(Scene& scene, int frames) {
    for (int i = 0; i < frames; ++i) scene.update(kDt);
}

// A 1 m cube of the given mass that neither falls nor damps, so every velocity
// below is exactly what the applied impulse or force put there.
RigidBodyNode* addFreeBody(Scene& scene, const char* name, float mass) {
    auto* body = scene.createChild<RigidBodyNode>();
    body->setName(name);
    body->mass = mass;
    body->gravityFactor = 0.0f;
    body->linearDamping = 0.0f;
    body->angularDamping = 0.0f;
    auto shape = std::make_unique<CollisionShapeNode>();
    shape->shapeType = CollisionShapeType::Box;
    shape->halfExtents = glm::vec3(0.5f);
    body->addChild(std::move(shape));
    return body;
}

// ---- impulses --------------------------------------------------------------

void testCentralImpulse() {
    Scene scene;
    auto* body = addFreeBody(scene, "Central", 2.0f);
    step(scene, 1);  // the body is created on the first update
    PhysicsWorld* world = scene.physics();
    require(world != nullptr, "scene has a physics world");
    require(!body->bodyId().IsInvalid(), "the rigid body reached the world");

    world->applyImpulse(body->bodyId(), glm::vec3(0.0f, 0.0f, 10.0f));

    // J/m = 10/2 = 5 m/s, and a central impulse must not rotate the body.
    const glm::vec3 v = world->linearVelocity(body->bodyId());
    requireNear(v.z, 5.0f, 1e-3f, "central impulse gives J/m along its own axis");
    requireNear(v.x, 0.0f, 1e-3f, "central impulse leaks nothing into x");
    requireNear(v.y, 0.0f, 1e-3f, "central impulse leaks nothing into y");
    const glm::vec3 w = world->angularVelocity(body->bodyId());
    requireNear(glm::length(w), 0.0f, 1e-3f, "central impulse adds no spin");

    // Nothing acts on the body afterwards, so the velocity survives the step.
    step(scene, 1);
    requireNear(world->linearVelocity(body->bodyId()).z, 5.0f, 1e-3f,
                "velocity from an impulse survives a step");
}

void testOffCentreImpulseSpins() {
    Scene scene;
    auto* body = addFreeBody(scene, "OffCentre", 2.0f);
    step(scene, 1);
    PhysicsWorld* world = scene.physics();

    // J = +10x applied at r = +0.5z. r x J = (0, 5, 0), so the box yaws about +Y
    // while its centre still gains J/m along +X.
    const glm::vec3 contact(0.0f, 0.0f, 0.5f);
    world->applyImpulse(body->bodyId(), glm::vec3(10.0f, 0.0f, 0.0f), contact);

    const glm::vec3 v = world->linearVelocity(body->bodyId());
    requireNear(v.x, 5.0f, 1e-3f, "an off-centre impulse still moves the centre by J/m");

    const glm::vec3 w = world->angularVelocity(body->bodyId());
    require(w.y > 0.1f, "an off-centre impulse spins the body about +Y");
    requireNear(w.x, 0.0f, 1e-3f, "the induced spin has no x component");
    requireNear(w.z, 0.0f, 1e-3f, "the induced spin has no z component");

    // The contact point is ahead of the centre on the spin, so it moves faster
    // than the centre along +X: that difference is what tyre slip is computed on.
    const glm::vec3 centre = world->pointVelocity(body->bodyId(), glm::vec3(0.0f));
    const glm::vec3 atContact = world->pointVelocity(body->bodyId(), contact);
    requireNear(centre.x, v.x, 1e-3f, "pointVelocity at the centre is the linear velocity");
    requireNear(atContact.x - centre.x, w.y * contact.z, 1e-3f,
                "pointVelocity adds the spin contribution w x r");
}

void testAngularImpulse() {
    Scene scene;
    auto* body = addFreeBody(scene, "Angular", 2.0f);
    step(scene, 1);
    PhysicsWorld* world = scene.physics();

    world->applyAngularImpulse(body->bodyId(), glm::vec3(0.0f, 1.0f, 0.0f));
    require(world->angularVelocity(body->bodyId()).y > 0.1f, "an angular impulse spins about +Y");
    requireNear(glm::length(world->linearVelocity(body->bodyId())), 0.0f, 1e-3f,
                "an angular impulse moves nothing linearly");
}

// ---- forces ----------------------------------------------------------------

void testForceIsIntegratedThenCleared() {
    Scene scene;
    auto* body = addFreeBody(scene, "Forced", 2.0f);
    step(scene, 1);
    PhysicsWorld* world = scene.physics();

    // A force is consumed by the step: F/m * dt = 12/2 * 1/60 = 0.1 m/s.
    world->applyForce(body->bodyId(), glm::vec3(0.0f, 0.0f, 12.0f));
    step(scene, 1);
    const float afterOneStep = world->linearVelocity(body->bodyId()).z;
    requireNear(afterOneStep, 0.1f, 1e-3f, "a force is integrated over one fixed step");

    // Jolt clears the accumulator after each step, so a force applied once does
    // not keep pushing. This is the trap the header warns about.
    step(scene, 5);
    requireNear(world->linearVelocity(body->bodyId()).z, afterOneStep, 1e-3f,
                "a force stops acting once its step has run");
}

void testTorque() {
    Scene scene;
    auto* body = addFreeBody(scene, "Torqued", 2.0f);
    step(scene, 1);
    PhysicsWorld* world = scene.physics();

    world->applyTorque(body->bodyId(), glm::vec3(0.0f, 6.0f, 0.0f));
    step(scene, 1);
    require(world->angularVelocity(body->bodyId()).y > 0.0f, "a torque spins the body about +Y");
}

// ---- bodies that must absorb it -------------------------------------------

void testStaticAndInvalidAreInert() {
    Scene scene;
    auto* wall = scene.createChild<StaticBodyNode>();
    wall->setName("Wall");
    auto shape = std::make_unique<CollisionShapeNode>();
    shape->shapeType = CollisionShapeType::Box;
    shape->halfExtents = glm::vec3(1.0f);
    wall->addChild(std::move(shape));
    step(scene, 1);
    PhysicsWorld* world = scene.physics();

    const glm::vec3 before = wall->transform().position;
    world->applyImpulse(wall->bodyId(), glm::vec3(0.0f, 0.0f, 1000.0f));
    world->applyForce(wall->bodyId(), glm::vec3(0.0f, 0.0f, 1000.0f));
    world->applyTorque(wall->bodyId(), glm::vec3(0.0f, 1000.0f, 0.0f));
    step(scene, 5);
    requireNear(glm::length(wall->transform().position - before), 0.0f, 1e-4f,
                "a static body ignores impulses, forces and torques");
    requireNear(glm::length(world->linearVelocity(wall->bodyId())), 0.0f, 1e-4f,
                "a static body keeps a zero velocity");

    // An invalid id is the normal case for a node whose body has not been built
    // yet; every entry point must tolerate it.
    const JPH::BodyID invalid;
    world->applyImpulse(invalid, glm::vec3(1.0f));
    world->applyImpulse(invalid, glm::vec3(1.0f), glm::vec3(1.0f));
    world->applyAngularImpulse(invalid, glm::vec3(1.0f));
    world->applyForce(invalid, glm::vec3(1.0f));
    world->applyForce(invalid, glm::vec3(1.0f), glm::vec3(1.0f));
    world->applyTorque(invalid, glm::vec3(1.0f));
    requireNear(glm::length(world->linearVelocity(invalid)), 0.0f, 1e-6f,
                "an invalid id reads back a zero velocity");
    requireNear(glm::length(world->pointVelocity(invalid, glm::vec3(1.0f))), 0.0f, 1e-6f,
                "pointVelocity tolerates an invalid id");
}

} // namespace

int main() {
    testCentralImpulse();
    testOffCentreImpulseSpins();
    testAngularImpulse();
    testForceIsIntegratedThenCleared();
    testTorque();
    testStaticAndInvalidAreInert();
    std::printf("[physics-impulse] PASS (%d checks)\n", gChecks);
    return 0;
}
