#pragma once

#include "core/Reflection.hpp"
#include "scene/Behaviour.hpp"

#include <glm/glm.hpp>

#include <string>

namespace saida {

// Third-person follow camera (cf. Cinemachine 3 ThirdPersonFollow + Deoccluder).
// Attach to a CameraNode: it orbits a target (found by group, never by name) with
// the mouse, sits behind it at a damped distance, and pulls in when a wall would
// occlude the view (physics raycast).
//
// Expected setup: a top-level CameraNode (no rotated/scaled parent), with this
// behaviour, and a target node in the `targetGroup` (default "player").
class CameraFollowBehaviour : public Behaviour {
public:
    void onReady() override;
    void onUpdate(float dt) override;

    // Target.
    std::string targetGroup = "player";

    // Rig geometry.
    float distance = 5.0f;         // resting distance behind the pivot
    float height = 1.6f;           // pivot height above the target origin
    float shoulderOffset = 0.0f;   // lateral offset (over-the-shoulder)

    // Orbit (mouse look).
    float yawSensitivity = 0.20f;   // degrees per pixel
    float pitchSensitivity = 0.18f;
    float minPitch = -35.0f;        // degrees
    float maxPitch = 70.0f;

    // Smoothing (exponential; higher = snappier).
    float positionDamping = 14.0f;

    // Wall handling.
    float collisionMargin = 0.25f;  // keep the camera this far off the hit surface
    float minDistance = 0.6f;       // never closer than this to the pivot

    SAIDA_REFLECT_BEHAVIOUR(CameraFollowBehaviour, "CameraFollow")

private:
    glm::vec3 computeDesired(const glm::vec3& pivot) const;

    float yaw_ = -90.0f;   // degrees (orbit angle around the target)
    float pitch_ = 15.0f;
    glm::vec3 camPos_{0.0f};
    bool initialized_ = false;
};

} // namespace saida
