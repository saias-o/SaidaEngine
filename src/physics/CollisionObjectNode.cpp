#include "physics/CollisionObjectNode.hpp"

#include "physics/CollisionShapeNode.hpp"
#include "physics/JoltGlue.hpp"
#include "core/Log.hpp"

#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <nlohmann/json.hpp>

#include <vector>

namespace saida {

CollisionObjectNode::~CollisionObjectNode() {
    detachFromPhysics();
}

void CollisionObjectNode::detachFromPhysics() {
    if (world_ && !bodyId_.IsInvalid()) {
        world_->removeBody(bodyId_);
    }
    bodyId_ = JPH::BodyID();
    world_ = nullptr;
}

void CollisionObjectNode::decomposeTR(const glm::mat4& w, glm::vec3& t, glm::quat& r,
                                      glm::mat4& invTR) {
    t = glm::vec3(w[3]);
    glm::vec3 c0(w[0]), c1(w[1]), c2(w[2]);
    glm::vec3 s(glm::length(c0), glm::length(c1), glm::length(c2));
    if (s.x < 1e-8f) s.x = 1.0f;
    if (s.y < 1e-8f) s.y = 1.0f;
    if (s.z < 1e-8f) s.z = 1.0f;
    glm::mat3 rot(c0 / s.x, c1 / s.y, c2 / s.z);
    r = glm::normalize(glm::quat_cast(rot));
    invTR = glm::inverse(glm::translate(glm::mat4(1.0f), t) * glm::mat4_cast(r));
}

void CollisionObjectNode::resolveAutoShapes() {
    glm::vec3 position;
    glm::quat rotation;
    glm::mat4 invTR;
    decomposeTR(worldTransform(), position, rotation, invTR);
    for (const auto& child : children()) {
        if (auto* cs = dynamic_cast<CollisionShapeNode*>(child.get())) {
            // A re-resolution (e.g. an async mesh's geometry has arrived) makes
            // the Jolt shape stale: the next syncToPhysics rebuilds it.
            if (cs->ensureResolved(invTR, *this)) markDirty();
        }
    }
}

JPH::Ref<JPH::Shape> CollisionObjectNode::buildCompoundShape(const glm::mat4& invBodyTR) {
    using namespace JPH;

    // Build a shape from each CollisionShape child (already offset into body space).
    std::vector<Ref<Shape>> shapes;
    for (const auto& child : children()) {
        if (auto* cs = dynamic_cast<CollisionShapeNode*>(child.get())) {
            if (Ref<Shape> s = cs->buildShape(invBodyTR, *this)) shapes.push_back(s);
        }
    }
    if (shapes.empty()) {
        // A mesh still loading asynchronously isn't an error: the body will
        // be (re)tried on every sync until the geometry arrives.
        bool pending = false;
        for (const auto& child : children())
            if (auto* cs = dynamic_cast<CollisionShapeNode*>(child.get()))
                pending = pending || cs->meshPending();
        if (!pending)
            Log::warn("CollisionObject '", name(), "' has no CollisionShape child — no shape built");
        return Ref<Shape>();
    }
    if (shapes.size() == 1) return shapes[0];

    StaticCompoundShapeSettings compound;
    for (Ref<Shape>& s : shapes)
        compound.AddShape(Vec3::sZero(), Quat::sIdentity(), s.GetPtr());
    ShapeSettings::ShapeResult r = compound.Create();
    if (!r.IsValid()) {
        Log::warn("CollisionObject '", name(), "': compound shape failed: ", r.GetError().c_str());
        return Ref<Shape>();
    }
    return r.Get();
}

void CollisionObjectNode::createBody(PhysicsWorld& world) {
    using namespace JPH;

    glm::vec3 position;
    glm::quat rotation;
    glm::mat4 invTR;
    decomposeTR(worldTransform(), position, rotation, invTR);

    Ref<Shape> finalShape = buildCompoundShape(invTR);
    if (finalShape == nullptr) return;

    BodyDesc desc;
    desc.shape = finalShape.GetPtr();
    desc.position = position;
    desc.rotation = rotation;
    desc.motion = motion();
    desc.isSensor = isSensor();
    desc.friction = friction;
    desc.restitution = restitution;
    desc.userData = this;
    describeBody(desc);  // subclass fills mass/damping/gravity for dynamic bodies

    bodyId_ = world.createBody(desc);
    world_ = &world;
}

void CollisionObjectNode::syncToPhysics(PhysicsWorld& world) {
    if (dirty_ && !bodyId_.IsInvalid()) {
        detachFromPhysics();  // params/shape changed → rebuild
    }
    dirty_ = false;

    if (bodyId_.IsInvalid()) {
        createBody(world);
        return;
    }

    // Static & kinematic bodies are driven by the node tree (e.g. scripted
    // platforms). Dynamic bodies own their transform — don't fight the solver.
    if (motion() != BodyMotion::Dynamic) {
        glm::vec3 position;
        glm::quat rotation;
        glm::mat4 invTR;
        decomposeTR(worldTransform(), position, rotation, invTR);
        world.setBodyTransform(bodyId_, position, rotation, motion() == BodyMotion::Kinematic);
    }
}

void CollisionObjectNode::syncFromPhysics(PhysicsWorld& world) {
    if (bodyId_.IsInvalid() || motion() != BodyMotion::Dynamic) return;

    glm::vec3 worldPos;
    glm::quat worldRot;
    world.getBodyTransform(bodyId_, worldPos, worldRot);
    writeWorldPoseToLocal(worldPos, worldRot);
}

void CollisionObjectNode::writeWorldPoseToLocal(const glm::vec3& worldPos,
                                                const glm::quat& worldRot) {
    // Convert a physics-driven world pose into this node's local transform,
    // leaving the node's scale untouched (physics never changes scale).
    glm::mat4 bodyWorld = glm::translate(glm::mat4(1.0f), worldPos) * glm::mat4_cast(worldRot);
    glm::mat4 parentWorld = parent() ? parent()->worldTransform() : glm::mat4(1.0f);
    glm::mat4 local = glm::inverse(parentWorld) * bodyWorld;

    glm::vec3 c0(local[0]), c1(local[1]), c2(local[2]);
    glm::vec3 s(glm::length(c0), glm::length(c1), glm::length(c2));
    if (s.x < 1e-8f) s.x = 1.0f;
    if (s.y < 1e-8f) s.y = 1.0f;
    if (s.z < 1e-8f) s.z = 1.0f;
    glm::mat3 rot(c0 / s.x, c1 / s.y, c2 / s.z);

    transform().position = glm::vec3(local[3]);
    transform().rotation = glm::normalize(glm::quat_cast(rot));
}

void CollisionObjectNode::serializeCommon(nlohmann::json& j) const {
    j["friction"] = friction;
    j["restitution"] = restitution;
}

void CollisionObjectNode::deserializeCommon(const nlohmann::json& j) {
    if (j.contains("friction")) friction = j["friction"].get<float>();
    if (j.contains("restitution")) restitution = j["restitution"].get<float>();
}

} // namespace saida
