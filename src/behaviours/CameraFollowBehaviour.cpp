#include "behaviours/CameraFollowBehaviour.hpp"

#include "scene/Node.hpp"
#include "scene/Scene.hpp"
#include "scene/SceneTree.hpp"
#include "physics/PhysicsWorld.hpp"
#include "core/Input.hpp"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>

namespace saida {

namespace {
constexpr glm::vec3 kWorldUp{0.0f, 1.0f, 0.0f};

// Look direction from yaw/pitch (same convention as Camera::front()).
glm::vec3 dirFromAngles(float yawDeg, float pitchDeg) {
    float y = glm::radians(yawDeg);
    float p = glm::radians(pitchDeg);
    return glm::normalize(glm::vec3(std::cos(y) * std::cos(p),
                                    std::sin(p),
                                    std::sin(y) * std::cos(p)));
}
}  // namespace

void CameraFollowBehaviour::onReady() {
    // Seed the orbit from the camera's current facing so it doesn't snap on start.
    glm::vec3 fwd = node()->worldTransform() * glm::vec4(0.0f, 0.0f, -1.0f, 0.0f);
    if (glm::dot(fwd, fwd) > 1e-6f) {
        fwd = glm::normalize(fwd);
        pitch_ = glm::degrees(std::asin(std::clamp(fwd.y, -1.0f, 1.0f)));
        yaw_ = glm::degrees(std::atan2(fwd.z, fwd.x));
    }
    camPos_ = glm::vec3(node()->worldTransform()[3]);
}

glm::vec3 CameraFollowBehaviour::computeDesired(const glm::vec3& pivot) const {
    glm::vec3 forward = dirFromAngles(yaw_, pitch_);          // camera looks toward pivot
    glm::vec3 right = glm::normalize(glm::cross(forward, kWorldUp));
    return pivot - forward * distance + right * shoulderOffset;
}

void CameraFollowBehaviour::onUpdate(float dt) {
    SceneTree* t = tree();
    if (!t) return;  // only meaningful at runtime
    Node* target = t->firstInGroup(targetGroup);
    if (!target) return;  // nothing to follow

    // Orbit from mouse movement (pitch clamped to avoid flipping).
    glm::vec2 m = Input::mouseDelta();
    yaw_ += m.x * yawSensitivity;
    pitch_ = std::clamp(pitch_ - m.y * pitchSensitivity, minPitch, maxPitch);

    glm::vec3 targetPos = glm::vec3(target->worldTransform()[3]);
    glm::vec3 pivot = targetPos + glm::vec3(0.0f, height, 0.0f);

    glm::vec3 desired = computeDesired(pivot);

    // Wall handling: cast from the pivot toward the desired camera position and pull
    // in if something is in the way. The root scene (the World in Play) owns the
    // physics world containing every collider.
    PhysicsWorld* physics = t->world().physics();
    if (physics) {
        glm::vec3 toCam = desired - pivot;
        float dist = glm::length(toCam);
        if (dist > 1e-4f) {
            glm::vec3 dir = toCam / dist;
            RaycastHit hit = physics->raycast(pivot, dir, dist);
            if (hit.hit) {
                float clamped = std::max(hit.distance - collisionMargin, minDistance);
                desired = pivot + dir * clamped;
            }
        }
    }

    // Exponential smoothing toward the desired position (frame-rate independent).
    float a = 1.0f - std::exp(-positionDamping * dt);
    if (!initialized_) { camPos_ = desired; initialized_ = true; }
    else camPos_ = glm::mix(camPos_, desired, a);

    // Drive the node: position + orientation looking at the pivot. Assumes a
    // top-level camera (local transform == world transform).
    node()->transform().position = camPos_;
    glm::vec3 lookDir = pivot - camPos_;
    if (glm::dot(lookDir, lookDir) > 1e-6f)
        node()->transform().rotation =
            glm::quat_cast(glm::inverse(glm::lookAt(camPos_, pivot, kWorldUp)));
}

void CameraFollowBehaviour::describe(reflect::TypeBuilder<CameraFollowBehaviour>& t) {
    t.doc("Third-person follow camera that orbits a target group and avoids walls.");
    t.property("targetGroup", &CameraFollowBehaviour::targetGroup)
        .tooltip("group tag of the node to follow");
    t.property("distance", &CameraFollowBehaviour::distance).range(0.5, 50.0);
    t.property("height", &CameraFollowBehaviour::height).range(0.0, 10.0);
    t.property("shoulderOffset", &CameraFollowBehaviour::shoulderOffset).range(-5.0, 5.0);
    t.property("yawSensitivity", &CameraFollowBehaviour::yawSensitivity).range(0.0, 2.0);
    t.property("pitchSensitivity", &CameraFollowBehaviour::pitchSensitivity).range(0.0, 2.0);
    t.property("minPitch", &CameraFollowBehaviour::minPitch).range(-89.0, 0.0);
    t.property("maxPitch", &CameraFollowBehaviour::maxPitch).range(0.0, 89.0);
    t.property("positionDamping", &CameraFollowBehaviour::positionDamping).range(1.0, 50.0);
    t.property("collisionMargin", &CameraFollowBehaviour::collisionMargin).range(0.0, 2.0);
    t.property("minDistance", &CameraFollowBehaviour::minDistance).range(0.1, 10.0);
}

} // namespace saida
