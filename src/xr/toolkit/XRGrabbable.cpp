#include "xr/toolkit/XRGrabbable.hpp"
#include "xr/toolkit/XRController.hpp"
#include "xr/toolkit/XRTypes.hpp"

#include "scene/Node.hpp"
#include "physics/CollisionObjectNode.hpp"
#include "physics/RigidBodyNode.hpp"

#include <glm/gtc/quaternion.hpp>
#include <nlohmann/json.hpp>

namespace saida {

void XRGrabbable::onReady() {
    node()->addToGroup(kXRGrabbableGroup);
    lastPos_ = glm::vec3(node()->worldTransform()[3]);
}

void XRGrabbable::grab(XRController* by) {
    if (!by || holder_ == by) return;
    if (holder_) release();
    holder_ = by;

    const glm::mat4 objWorld = node()->worldTransform();
    grabOffset_ = snapToHand ? glm::mat4(1.0f)
                            : glm::inverse(by->worldTransform()) * objWorld;
    lastPos_ = glm::vec3(objWorld[3]);
    throwVelocity_ = glm::vec3(0.0f);

    // Physics-aware: hold a RigidBody as kinematic so it tracks the hand and still
    // pushes other dynamic bodies, then restore + throw on release.
    if (auto* co = node()->asCollisionObject()) {
        if (auto* rb = dynamic_cast<RigidBodyNode*>(co); rb && !rb->kinematic) {
            restoreDynamic_ = true;
            rb->kinematic = true;
            rb->markDirty();
        }
    }
    grabbed.emit(by);
}

void XRGrabbable::release() {
    if (!holder_) return;
    XRController* by = holder_;
    holder_ = nullptr;

    if (restoreDynamic_) {
        restoreDynamic_ = false;
        if (auto* co = node()->asCollisionObject()) {
            if (auto* rb = dynamic_cast<RigidBodyNode*>(co)) {
                rb->kinematic = false;
                rb->markDirty();
                pendingThrow_ = true;  // body rebuilds this sync; throw next frame
            }
        }
    }
    released.emit(by);
}

void XRGrabbable::onUpdate(float dt) {
    // Apply a deferred throw one frame after release (the dynamic body now exists).
    if (pendingThrow_) {
        if (auto* co = node()->asCollisionObject()) co->setLinearVelocity(throwVelocity_);
        pendingThrow_ = false;
    }
    if (!holder_) return;

    const glm::mat4 desiredWorld = holder_->worldTransform() * grabOffset_;
    const glm::mat4 parentWorld =
        node()->parent() ? node()->parent()->worldTransform() : glm::mat4(1.0f);
    const glm::mat4 local = glm::inverse(parentWorld) * desiredWorld;

    node()->transform().position = glm::vec3(local[3]);
    node()->transform().rotation = glm::normalize(glm::quat_cast(glm::mat3(local)));

    // Track hand-driven world velocity (lightly smoothed) to throw on release.
    const glm::vec3 worldPos = glm::vec3(desiredWorld[3]);
    if (dt > 0.0f)
        throwVelocity_ = glm::mix(throwVelocity_, (worldPos - lastPos_) / dt, 0.5f);
    lastPos_ = worldPos;
}

void XRGrabbable::onDestroy() {
    if (holder_) release();
}

void XRGrabbable::save(nlohmann::json& j) const {
    j["grabRadius"] = grabRadius;
    j["snapToHand"] = snapToHand;
}

void XRGrabbable::load(const nlohmann::json& j) {
    if (j.contains("grabRadius")) grabRadius = j["grabRadius"].get<float>();
    if (j.contains("snapToHand")) snapToHand = j["snapToHand"].get<bool>();
}

} // namespace saida
