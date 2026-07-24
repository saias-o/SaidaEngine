#pragma once

#include "scene/Node.hpp"
#include "physics/PhysicsWorld.hpp"  // BodyMotion + JPH::BodyID
#include "core/Signal.hpp"

namespace saida {

// Base of the physics body nodes (Godot-style). Holds a Jolt body built from the
// CollisionShape children. Subclasses declare their motion type.
class CollisionObjectNode : public Node {
public:
    using Node::Node;
    ~CollisionObjectNode() override;

    CollisionObjectNode* asCollisionObject() override { return this; }

    virtual BodyMotion motion() const = 0;
    virtual bool isSensor() const { return false; }

    // Called by Scene each frame: create the body if needed, then for non-dynamic
    // bodies push the node's (possibly script-driven) transform into Jolt.
    virtual void syncToPhysics(PhysicsWorld& world);
    // Called between syncToPhysics and the solver step; characters move here.
    virtual void prePhysicsStep(PhysicsWorld& /*world*/, float /*dt*/) {}
    // Called after the step: for dynamic bodies, write Jolt's result back into
    // the node's local transform.
    virtual void syncFromPhysics(PhysicsWorld& world);

    // Resolve any Auto CollisionShape children once, using the body's current
    // (fresh) world transform. Called by the Scene every frame; cheap once frozen.
    void resolveAutoShapes();

    // Drop the Jolt body (on disable / removal from a physics scene).
    void detachFromPhysics();
    // Force a rebuild of the body next sync (after a shape/param change).
    void markDirty() { dirty_ = true; }

    JPH::BodyID bodyId() const { return bodyId_; }

    // Set this body's velocity (no-op until it has a live Jolt body). Used by grab
    // release to throw a freed object with the hand's motion.
    void setLinearVelocity(const glm::vec3& v) {
        if (world_ && !bodyId_.IsInvalid()) world_->setLinearVelocity(bodyId_, v);
    }
    void setAngularVelocity(const glm::vec3& v) {
        if (world_ && !bodyId_.IsInvalid()) world_->setAngularVelocity(bodyId_, v);
    }

    // Fired (main thread) when this body starts/stops touching another solid body.
    // Sensors (Area) use bodyEntered/bodyExited instead. Listen with
    // `behaviour->listen(body->collisionEntered, [](CollisionObjectNode* other){...})`.
    Signal<CollisionObjectNode*> collisionEntered;
    Signal<CollisionObjectNode*> collisionExited;

    float friction = 0.5f;
    float restitution = 0.0f;

protected:
    // Subclasses fill in the dynamic-only fields of the desc (mass, damping…).
    virtual void describeBody(BodyDesc& /*desc*/) const {}

    // Save/load the fields common to every body (friction, restitution).
    void serializeCommon(nlohmann::json& j) const;
    void deserializeCommon(const nlohmann::json& j);

    // Build one Jolt shape from the CollisionShape children (compound if several),
    // expressed in the body's local unscaled frame. Null if no shape child.
    JPH::Ref<JPH::Shape> buildCompoundShape(const glm::mat4& invBodyTR);

    // Split a world matrix into translation + rotation (scale is baked into the
    // shape — Jolt bodies are unscaled) plus the inverse of that TR.
    static void decomposeTR(const glm::mat4& world, glm::vec3& t, glm::quat& r,
                            glm::mat4& invTR);
    // Write a physics-driven world pose back into this node's local transform,
    // preserving the node's scale.
    void writeWorldPoseToLocal(const glm::vec3& worldPos, const glm::quat& worldRot);

    JPH::BodyID bodyId_;
    PhysicsWorld* world_ = nullptr;
    bool dirty_ = false;

private:
    void createBody(PhysicsWorld& world);
};

} // namespace saida
