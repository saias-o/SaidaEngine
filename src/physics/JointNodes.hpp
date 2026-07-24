#pragma once

// Physics joint nodes (V1 set): Fixed, Point and Hinge constraints between two
// physics bodies, authored as scene nodes (Godot Joint3D-style).
//
// A joint references its bodies by *node path* resolved from the joint node
// ("../Crate", "Door", "/Arena/Gate"): paths survive renaming-agnostic spawning
// (`tree.instantiate` regenerates NodeIds, which is why serialized ids are not
// used here). An empty `bodyA` defaults to the nearest CollisionObjectNode
// ancestor — the natural authoring shape is the joint as a child of body A. An
// empty `bodyB` anchors body A to the world.
//
// Lifecycle: the Scene calls syncJointToPhysics() every frame after the bodies
// synced. The joint builds its Jolt constraint once both bodies are live, and
// rebuilds it automatically when a referenced body was recreated (shape edit →
// markDirty → new BodyID). PhysicsWorld drops any constraint attached to a
// body being removed, so a joint can never dangle.
#include "scene/Node.hpp"
#include "core/Reflection.hpp"
#include "physics/PhysicsWorld.hpp"

#include <Jolt/Physics/Constraints/TwoBodyConstraint.h>

namespace saida {

class CollisionObjectNode;

class JointNode : public Node {
public:
    using Node::Node;
    ~JointNode() override;

    JointNode* asJointNode() override { return this; }

    // Called by the Scene each physics frame, after body sync (bodies exist).
    void syncJointToPhysics(PhysicsWorld& world);
    // Drop the live constraint (scene teardown / joint disabled).
    void detachJointFromPhysics();

    // True while a Jolt constraint is live (both bodies resolved and joined).
    bool jointActive() const { return constraint_ != nullptr; }

    // Serialized body references (see file comment for path semantics).
    std::string bodyA;
    std::string bodyB;

protected:
    // Build the concrete Jolt constraint for the two resolved bodies. `world`
    // pivot data comes from this node's own world transform.
    virtual JPH::Ref<JPH::TwoBodyConstraint> buildConstraint(JPH::Body& a,
                                                             JPH::Body& b) = 0;

    // Resolve a body reference to the node's live Jolt body id (invalid when
    // the path or the body does not resolve yet).
    CollisionObjectNode* resolveBody(const std::string& path, bool defaultToAncestor);

private:
    PhysicsWorld* world_ = nullptr;
    JPH::Ref<JPH::TwoBodyConstraint> constraint_;
    // Body ids the live constraint was built against; a mismatch means a body
    // was rebuilt and the constraint must be recreated.
    JPH::BodyID builtA_;
    JPH::BodyID builtB_;
};

// Welds two bodies at their current relative pose.
class FixedJointNode : public JointNode {
public:
    FixedJointNode() : JointNode("FixedJoint") {}
    SAIDA_REFLECT_NODE(FixedJointNode, "FixedJoint")

protected:
    JPH::Ref<JPH::TwoBodyConstraint> buildConstraint(JPH::Body& a, JPH::Body& b) override;
};

// Ball-socket: both bodies pivot freely around this node's world position.
class PointJointNode : public JointNode {
public:
    PointJointNode() : JointNode("PointJoint") {}
    SAIDA_REFLECT_NODE(PointJointNode, "PointJoint")

protected:
    JPH::Ref<JPH::TwoBodyConstraint> buildConstraint(JPH::Body& a, JPH::Body& b) override;
};

// Rotation around a single axis through this node's world position (doors,
// wheels, levers). Optional angular limits in degrees.
class HingeJointNode : public JointNode {
public:
    HingeJointNode() : JointNode("HingeJoint") {}
    SAIDA_REFLECT_NODE(HingeJointNode, "HingeJoint")

    glm::vec3 axis{0.0f, 1.0f, 0.0f};  // local to this node; normalized on use
    bool limitEnabled = false;
    float limitMinDeg = -45.0f;
    float limitMaxDeg = 45.0f;

protected:
    JPH::Ref<JPH::TwoBodyConstraint> buildConstraint(JPH::Body& a, JPH::Body& b) override;
};

} // namespace saida
