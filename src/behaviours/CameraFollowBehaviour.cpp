#include "behaviours/CameraFollowBehaviour.hpp"

#include "nodes/CameraNode.hpp"
#include "scene/Node.hpp"
#include "scene/Scene.hpp"
#include "scene/SceneTree.hpp"
#include "physics/CollisionObjectNode.hpp"
#include "physics/PhysicsWorld.hpp"
#include "core/Input.hpp"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>

namespace saida {

namespace {
constexpr glm::vec3 kWorldUp{0.0f, 1.0f, 0.0f};

// Occlusion hits closer than this are the caster answering its own ray, not a
// wall. See the note at the raycast below.
constexpr float kSelfHitEpsilon = 0.05f;

// Look direction from yaw/pitch (same convention as Camera::front()).
glm::vec3 dirFromAngles(float yawDeg, float pitchDeg) {
    float y = glm::radians(yawDeg);
    float p = glm::radians(pitchDeg);
    return glm::normalize(glm::vec3(std::cos(y) * std::cos(p),
                                    std::sin(p),
                                    std::sin(y) * std::cos(p)));
}

// Shortest signed distance from `from` to `to`, in degrees.
float angleDelta(float from, float to) {
    float d = std::fmod(to - from + 180.0f, 360.0f);
    if (d < 0.0f) d += 360.0f;
    return d - 180.0f;
}

float damp(float current, float target, float rate, float dt) {
    if (rate <= 0.0f) return target;
    return glm::mix(current, target, 1.0f - std::exp(-rate * dt));
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
    if (initialPitch != 0.0f) pitch_ = std::clamp(initialPitch, minPitch, maxPitch);
    camPos_ = glm::vec3(node()->worldTransform()[3]);
    if (auto* cam = dynamic_cast<CameraNode*>(node())) fov_ = cam->fovDegrees;
}

glm::vec2 CameraFollowBehaviour::readOrbitInput() {
    const glm::vec2 m = Input::mouseDelta();
    return {m.x * yawSensitivity,
            m.y * pitchSensitivity * (invertPitch ? 1.0f : -1.0f)};
}

void CameraFollowBehaviour::orbit(float yawDegrees, float pitchDegrees) {
    yaw_ += yawDegrees;
    pitch_ = std::clamp(pitch_ + pitchDegrees, minPitch, maxPitch);
    idleTimer_ = 0.0f;
    recentring_ = false;
}

void CameraFollowBehaviour::setYaw(float yawDegrees) { yaw_ = yawDegrees; }
void CameraFollowBehaviour::setPitch(float pitchDegrees) {
    pitch_ = std::clamp(pitchDegrees, minPitch, maxPitch);
}

void CameraFollowBehaviour::recenter() { recentring_ = true; }

void CameraFollowBehaviour::snap() { initialized_ = false; }

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
    if (dt <= 0.0f) return;

    // --- orbit ---------------------------------------------------------------
    const glm::vec2 orbitInput = readOrbitInput();
    if (std::abs(orbitInput.x) > 1e-4f || std::abs(orbitInput.y) > 1e-4f) {
        yaw_ += orbitInput.x;
        pitch_ = std::clamp(pitch_ + orbitInput.y, minPitch, maxPitch);
        idleTimer_ = 0.0f;
        recentring_ = false;
    } else {
        idleTimer_ += dt;
        if (recenterDelay > 0.0f && idleTimer_ >= recenterDelay) recentring_ = true;
    }

    const glm::vec3 targetPos = glm::vec3(target->worldTransform()[3]);
    // Measured, not asked for, so it works for anything that moves — character,
    // vehicle, projectile. Needed before the recentring decision below.
    glm::vec3 targetVelocity = initialized_ ? (targetPos - lastTargetPos_) / dt : glm::vec3(0.0f);
    targetVelocity.y = 0.0f;
    const float targetSpeed = glm::length(targetVelocity);

    // Drift back behind the target's own facing. The rig's yaw is the direction
    // the camera looks in, which behind the target is the target's forward.
    if (recentring_ && targetSpeed >= recenterMinSpeed) {
        glm::vec3 fwd = recenterOnVelocity && targetSpeed > 1e-3f
            ? targetVelocity
            : glm::vec3(target->worldTransform() * glm::vec4(0.0f, 0.0f, -1.0f, 0.0f));
        fwd.y = 0.0f;
        if (glm::dot(fwd, fwd) > 1e-6f) {
            fwd = glm::normalize(fwd);
            const float wanted = glm::degrees(std::atan2(fwd.z, fwd.x));
            const float delta = angleDelta(yaw_, wanted);
            const float step = recenterSpeed * dt;
            yaw_ += std::abs(delta) <= step ? delta : (delta > 0.0f ? step : -step);
        }
        if (recenterPitchToo) {
            const float pitchDelta = recenterPitch - pitch_;
            const float pitchStep = recenterSpeed * dt;
            pitch_ = std::clamp(
                pitch_ + (std::abs(pitchDelta) <= pitchStep
                              ? pitchDelta
                              : (pitchDelta > 0.0f ? pitchStep : -pitchStep)),
                minPitch, maxPitch);
        }
    }

    // --- pivot ---------------------------------------------------------------
    // Vertical apart from horizontal: a camera that rides every jump bounces
    // the whole screen.
    const float wantedHeight = targetPos.y + height;
    if (!initialized_) pivotHeight_ = wantedHeight;
    if (verticalDamping <= 0.0f && verticalDeadZone <= 0.0f) {
        // Neither vertical parameter is set: the pivot tracks the target exactly
        // and the only smoothing is the rig's, which is what it did before.
        pivotHeight_ = wantedHeight;
    } else {
        float heightError = wantedHeight - pivotHeight_;
        if (verticalDeadZone > 0.0f) {
            if (std::abs(heightError) <= verticalDeadZone) heightError = 0.0f;
            else heightError -= (heightError > 0.0f ? verticalDeadZone : -verticalDeadZone);
        }
        const float rate = verticalDamping > 0.0f ? verticalDamping : positionDamping;
        pivotHeight_ = damp(pivotHeight_, pivotHeight_ + heightError, rate, dt);
    }

    if (lookAhead > 0.0f) {
        glm::vec3 wanted = targetVelocity * lookAhead;
        const float len = glm::length(wanted);
        if (len > lookAheadMaxDistance) wanted *= lookAheadMaxDistance / len;
        lookAheadOffset_.x = damp(lookAheadOffset_.x, wanted.x, lookAheadDamping, dt);
        lookAheadOffset_.z = damp(lookAheadOffset_.z, wanted.z, lookAheadDamping, dt);
    } else {
        lookAheadOffset_ = glm::vec3(0.0f);
    }

    const glm::vec3 pivot(targetPos.x + lookAheadOffset_.x, pivotHeight_,
                          targetPos.z + lookAheadOffset_.z);

    const glm::vec3 desired = computeDesired(pivot);

    // Exponential smoothing toward the desired position (frame-rate independent).
    if (!initialized_) {
        camPos_ = desired;
        initialized_ = true;
    } else {
        camPos_ = glm::mix(camPos_, desired, 1.0f - std::exp(-positionDamping * dt));
    }
    lastTargetPos_ = targetPos;

    // Applied AFTER the smoothing, not to the target it eases toward: pulling
    // in must be immediate or the camera spends the easing frames inside the
    // wall. Easing out is free — the clamp simply stops applying.
    if (PhysicsWorld* physics = t->world().physics()) {
        glm::vec3 toCam = camPos_ - pivot;
        const float dist = glm::length(toCam);
        if (dist > 1e-4f) {
            const glm::vec3 dir = toCam / dist;
            // The pivot sits inside the target, so the target answers its own
            // ray. The filter covers a RigidBody or StaticBody; a CharacterBody
            // needs the epsilon too, its inner body (SPEC 5.1) not being the one
            // named here and answering at distance zero.
            QueryFilter filter;
            if (auto* collider = dynamic_cast<CollisionObjectNode*>(target))
                filter.ignore = collider->bodyId();
            const RaycastHit hit = physics->raycast(pivot, dir, dist, filter);
            if (hit.hit && hit.distance > kSelfHitEpsilon)
                camPos_ = pivot + dir * std::max(hit.distance - collisionMargin, minDistance);
        }
    }

    // Drive the node: position + orientation looking at the pivot. Assumes a
    // top-level camera (local transform == world transform).
    node()->transform().position = camPos_;
    glm::vec3 lookDir = pivot - camPos_;
    if (glm::dot(lookDir, lookDir) > 1e-6f)
        node()->transform().rotation =
            glm::quat_cast(glm::inverse(glm::lookAt(camPos_, pivot, kWorldUp)));

    // Speed opens the lens. Left alone unless fovAtRest names a value, because
    // the camera's own fov is the authority otherwise.
    if (fovAtRest > 0.0f) {
        if (auto* cam = dynamic_cast<CameraNode*>(node())) {
            const float ratio = fovSpeedReference > 0.0f
                ? std::clamp(targetSpeed / fovSpeedReference, 0.0f, 1.0f)
                : 0.0f;
            const float wanted = glm::mix(fovAtRest, fovAtSpeed > 0.0f ? fovAtSpeed : fovAtRest, ratio);
            fov_ = damp(fov_ > 0.0f ? fov_ : wanted, wanted, fovDamping, dt);
            cam->fovDegrees = fov_;
        }
    }
}

void CameraFollowBehaviour::describe(reflect::TypeBuilder<CameraFollowBehaviour>& t) {
    t.doc("Third-person follow camera that orbits a target group, leads its motion, "
          "recentres behind it and avoids walls. Every parameter past the basic rig "
          "defaults to off, so an existing camera keeps its feel until it opts in.");

    t.property("targetGroup", &CameraFollowBehaviour::targetGroup)
        .group("Target").tooltip("group tag of the node to follow");
    t.property("distance", &CameraFollowBehaviour::distance).range(0.5, 50.0).group("Rig");
    t.property("height", &CameraFollowBehaviour::height).range(0.0, 10.0).group("Rig");
    t.property("shoulderOffset", &CameraFollowBehaviour::shoulderOffset).range(-5.0, 5.0).group("Rig");

    t.property("yawSensitivity", &CameraFollowBehaviour::yawSensitivity).range(0.0, 2.0).group("Orbit");
    t.property("pitchSensitivity", &CameraFollowBehaviour::pitchSensitivity).range(0.0, 2.0).group("Orbit");
    t.property("minPitch", &CameraFollowBehaviour::minPitch).range(-89.0, 0.0).group("Orbit");
    t.property("maxPitch", &CameraFollowBehaviour::maxPitch).range(0.0, 89.0).group("Orbit");
    t.property("invertPitch", &CameraFollowBehaviour::invertPitch).group("Orbit");
    t.property("initialPitch", &CameraFollowBehaviour::initialPitch).range(-89.0, 89.0)
        .group("Orbit").tooltip("pitch the rig starts at; NEGATIVE raises the camera above the target. 0 = keep the camera node's own orientation");

    t.property("positionDamping", &CameraFollowBehaviour::positionDamping).range(1.0, 50.0)
        .group("Damping").tooltip("horizontal follow rate; higher = snappier");
    t.property("verticalDamping", &CameraFollowBehaviour::verticalDamping).range(0.0, 50.0)
        .group("Damping").tooltip("vertical follow rate; 0 = same as positionDamping. Lower it so jumps do not shake the view");
    t.property("verticalDeadZone", &CameraFollowBehaviour::verticalDeadZone).range(0.0, 5.0)
        .group("Damping").tooltip("metres of vertical movement ignored entirely; 0 = disabled");

    t.property("lookAhead", &CameraFollowBehaviour::lookAhead).range(0.0, 1.0)
        .group("Look-ahead").tooltip("metres of lead per m/s of target speed; 0 = disabled");
    t.property("lookAheadMaxDistance", &CameraFollowBehaviour::lookAheadMaxDistance).range(0.0, 20.0)
        .group("Look-ahead");
    t.property("lookAheadDamping", &CameraFollowBehaviour::lookAheadDamping).range(0.5, 50.0)
        .group("Look-ahead");

    t.property("recenterDelay", &CameraFollowBehaviour::recenterDelay).range(0.0, 10.0)
        .group("Recentre").tooltip("seconds of no orbit input before drifting behind the target; 0 = never");
    t.property("recenterSpeed", &CameraFollowBehaviour::recenterSpeed).range(0.0, 720.0)
        .group("Recentre").tooltip("degrees per second while recentring");
    t.property("recenterPitchToo", &CameraFollowBehaviour::recenterPitchToo)
        .group("Recentre").tooltip("also drag the pitch back while recentring; off by default because it undoes the player's own adjustments");
    t.property("recenterPitch", &CameraFollowBehaviour::recenterPitch).range(-89.0, 89.0)
        .group("Recentre").tooltip("pitch the camera settles to when recenterPitchToo is on; NEGATIVE is above the target");
    t.property("recenterMinSpeed", &CameraFollowBehaviour::recenterMinSpeed).range(0.0, 20.0)
        .group("Recentre").tooltip("only swing behind a target moving at least this fast; 0 = always");
    t.property("recenterOnVelocity", &CameraFollowBehaviour::recenterOnVelocity)
        .group("Recentre").tooltip("swing behind the direction of travel rather than the target's facing");

    t.property("fovAtRest", &CameraFollowBehaviour::fovAtRest).range(0.0, 179.0)
        .group("Lens").tooltip("field of view when the target is still; 0 = leave the camera's own fov alone");
    t.property("fovAtSpeed", &CameraFollowBehaviour::fovAtSpeed).range(0.0, 179.0)
        .group("Lens").tooltip("field of view at fovSpeedReference");
    t.property("fovSpeedReference", &CameraFollowBehaviour::fovSpeedReference).range(0.1, 50.0)
        .group("Lens").tooltip("target speed at which fovAtSpeed is reached");
    t.property("fovDamping", &CameraFollowBehaviour::fovDamping).range(0.1, 50.0).group("Lens");

    t.property("collisionMargin", &CameraFollowBehaviour::collisionMargin).range(0.0, 2.0).group("Walls");
    t.property("minDistance", &CameraFollowBehaviour::minDistance).range(0.1, 10.0).group("Walls");
}

} // namespace saida
