#include "physics/JointNodes.hpp"

#include "core/Log.hpp"
#include "physics/CollisionObjectNode.hpp"
#include "physics/JoltGlue.hpp"

#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Body/BodyLockMulti.h>
#include <Jolt/Physics/Constraints/FixedConstraint.h>
#include <Jolt/Physics/Constraints/HingeConstraint.h>
#include <Jolt/Physics/Constraints/PointConstraint.h>

#include <glm/gtc/matrix_inverse.hpp>

namespace saida {

using namespace JPH;

JointNode::~JointNode() { detachJointFromPhysics(); }

void JointNode::detachJointFromPhysics() {
    if (world_ && constraint_) world_->removeConstraint(constraint_);
    constraint_ = nullptr;
    world_ = nullptr;
    builtA_ = BodyID();
    builtB_ = BodyID();
}

CollisionObjectNode* JointNode::resolveBody(const std::string& path,
                                            bool defaultToAncestor) {
    if (path.empty()) {
        if (!defaultToAncestor) return nullptr;
        for (Node* n = parent(); n; n = n->parent())
            if (CollisionObjectNode* body = n->asCollisionObject()) return body;
        return nullptr;
    }
    Node* target = findByPath(path);
    return target ? target->asCollisionObject() : nullptr;
}

void JointNode::syncJointToPhysics(PhysicsWorld& world) {
    world_ = &world;

    CollisionObjectNode* nodeA = resolveBody(bodyA, /*defaultToAncestor=*/true);
    CollisionObjectNode* nodeB = resolveBody(bodyB, /*defaultToAncestor=*/false);

    // Body A is mandatory; body B empty/unresolved → anchored to the world.
    const BodyID idA = nodeA ? nodeA->bodyId() : BodyID();
    const BodyID idB = nodeB ? nodeB->bodyId() : BodyID();

    if (constraint_) {
        // Rebuild when a referenced body was recreated (markDirty → new id) or
        // disappeared. PhysicsWorld already dropped the constraint if one of
        // its bodies was removed, but our ref still knows the stale ids.
        if (idA == builtA_ && idB == builtB_ && !idA.IsInvalid()) return;
        detachJointFromPhysics();
        world_ = &world;
    }

    if (!nodeA || idA.IsInvalid()) return;              // wait until A is live
    if (!bodyB.empty() && (!nodeB || idB.IsInvalid())) return;  // wait for B

    PhysicsSystem& system = world.system();
    Ref<TwoBodyConstraint> built;
    if (idB.IsInvalid()) {
        // World anchor: Jolt's sentinel body joins A to the static world.
        BodyLockWrite lock(system.GetBodyLockInterface(), idA);
        if (!lock.Succeeded()) return;
        built = buildConstraint(Body::sFixedToWorld, lock.GetBody());
    } else {
        const BodyID ids[2] = {idA, idB};
        BodyLockMultiWrite lock(system.GetBodyLockInterface(), ids, 2);
        Body* a = lock.GetBody(0);
        Body* b = lock.GetBody(1);
        if (!a || !b) return;
        built = buildConstraint(*a, *b);
    }
    if (!built) {
        Log::warn("JointNode '", name(), "': constraint creation failed");
        return;
    }
    world.addConstraint(built);
    constraint_ = std::move(built);
    builtA_ = idA;
    builtB_ = idB;
}

// ---- concrete joints -------------------------------------------------------

JPH::Ref<JPH::TwoBodyConstraint> FixedJointNode::buildConstraint(Body& a, Body& b) {
    FixedConstraintSettings settings;
    settings.mAutoDetectPoint = true;  // weld at the bodies' current relative pose
    return settings.Create(a, b);
}

JPH::Ref<JPH::TwoBodyConstraint> PointJointNode::buildConstraint(Body& a, Body& b) {
    const glm::vec3 pivot = glm::vec3(worldTransform()[3]);
    PointConstraintSettings settings;
    settings.mSpace = EConstraintSpace::WorldSpace;
    settings.mPoint1 = settings.mPoint2 = RVec3(pivot.x, pivot.y, pivot.z);
    return settings.Create(a, b);
}

JPH::Ref<JPH::TwoBodyConstraint> HingeJointNode::buildConstraint(Body& a, Body& b) {
    const glm::vec3 pivot = glm::vec3(worldTransform()[3]);
    // Axis is authored local to the joint node → world space (rotation only).
    glm::vec3 worldAxis = glm::mat3(worldTransform()) * axis;
    if (glm::dot(worldAxis, worldAxis) < 1e-8f) worldAxis = glm::vec3(0.0f, 1.0f, 0.0f);
    worldAxis = glm::normalize(worldAxis);

    HingeConstraintSettings settings;
    settings.mSpace = EConstraintSpace::WorldSpace;
    settings.mPoint1 = settings.mPoint2 = RVec3(pivot.x, pivot.y, pivot.z);
    settings.mHingeAxis1 = settings.mHingeAxis2 = toJolt(worldAxis);
    const Vec3 normal = settings.mHingeAxis1.GetNormalizedPerpendicular();
    settings.mNormalAxis1 = settings.mNormalAxis2 = normal;
    if (limitEnabled) {
        settings.mLimitsMin = glm::radians(limitMinDeg);
        settings.mLimitsMax = glm::radians(limitMaxDeg);
    }
    return settings.Create(a, b);
}

// ---- reflection ------------------------------------------------------------

void FixedJointNode::describe(reflect::TypeBuilder<FixedJointNode>& t) {
    t.doc("Welds two physics bodies at their current relative pose. Body A "
          "defaults to the nearest ancestor body; empty body B anchors to the "
          "world.");
    t.property("bodyA", static_cast<std::string FixedJointNode::*>(&FixedJointNode::bodyA))
        .tooltip("Node path of body A (empty = nearest ancestor body)");
    t.property("bodyB", static_cast<std::string FixedJointNode::*>(&FixedJointNode::bodyB))
        .tooltip("Node path of body B (empty = anchored to the world)");
}

void PointJointNode::describe(reflect::TypeBuilder<PointJointNode>& t) {
    t.doc("Ball-socket joint: the bodies pivot freely around this node's world "
          "position.");
    t.property("bodyA", static_cast<std::string PointJointNode::*>(&PointJointNode::bodyA))
        .tooltip("Node path of body A (empty = nearest ancestor body)");
    t.property("bodyB", static_cast<std::string PointJointNode::*>(&PointJointNode::bodyB))
        .tooltip("Node path of body B (empty = anchored to the world)");
}

void HingeJointNode::describe(reflect::TypeBuilder<HingeJointNode>& t) {
    t.doc("Hinge joint: rotation around one axis through this node's world "
          "position (doors, wheels, levers). Optional angular limits.");
    t.property("bodyA", static_cast<std::string HingeJointNode::*>(&HingeJointNode::bodyA))
        .tooltip("Node path of body A (empty = nearest ancestor body)");
    t.property("bodyB", static_cast<std::string HingeJointNode::*>(&HingeJointNode::bodyB))
        .tooltip("Node path of body B (empty = anchored to the world)");
    t.property("axis", &HingeJointNode::axis)
        .tooltip("Hinge axis, local to this node");
    t.property("limitEnabled", &HingeJointNode::limitEnabled)
        .tooltip("Clamp rotation between limitMinDeg and limitMaxDeg");
    t.property("limitMinDeg", &HingeJointNode::limitMinDeg).range(-180.0, 180.0);
    t.property("limitMaxDeg", &HingeJointNode::limitMaxDeg).range(-180.0, 180.0);
}

} // namespace saida
