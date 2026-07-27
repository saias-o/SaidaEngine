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
//
// Like CharacterBehaviour: every parameter defaults to the behaviour that
// existed before it, and the same three entry points apply — properties,
// functions (`orbit`, `setYaw`, `recenter`, `snap`), or `readOrbitInput`.
class CameraFollowBehaviour : public Behaviour {
public:
    ~CameraFollowBehaviour() override = default;

    void onReady() override;
    void onUpdate(float dt) override;

    // Target.
    std::string targetGroup = "player";

    // Rig geometry.
    float distance = 5.0f;         // resting distance behind the pivot
    float height = 1.6f;           // pivot height above the target origin
    float shoulderOffset = 0.0f;   // lateral offset (over-the-shoulder)

    // Orbit (mouse look). Pitch sign is easy to get backwards: the rig sits at
    // `pivot - forward * distance`, so a POSITIVE pitch puts the camera BELOW
    // its target and a negative one raises it.
    float yawSensitivity = 0.20f;   // degrees per pixel of mouse movement
    float pitchSensitivity = 0.18f;
    // Stick look, in degrees per second at full deflection — a rate, where the
    // mouse gives a displacement. Reads the Look* actions, which the default
    // bindings put on the right stick. 0 disables that source.
    float stickYawSpeed = 200.0f;
    float stickPitchSpeed = 140.0f;
    float minPitch = -35.0f;        // degrees; the lower bound, i.e. how HIGH the camera may go
    float maxPitch = 70.0f;         // how far BELOW the target it may drop
    bool invertPitch = false;
    // Pitch the rig starts at, overriding the camera node's own orientation.
    // 0 keeps that orientation, which is what the rig did before.
    float initialPitch = 0.0f;

    // Smoothing (exponential; higher = snappier).
    float positionDamping = 14.0f;
    // Keeps the horizon steady while the character bounces. 0 = positionDamping.
    float verticalDamping = 0.0f;
    float verticalDeadZone = 0.0f;  // vertical movement ignored entirely; 0 = off

    // Push the pivot along the target's velocity, so the player sees ahead.
    float lookAhead = 0.0f;          // metres per (m/s) of target speed; 0 = off
    float lookAheadMaxDistance = 3.0f;
    float lookAheadDamping = 6.0f;

    // Auto-recentre: the orbit drifts back behind the target once the mouse has
    // been still this long. 0 disables it.
    float recenterDelay = 0.0f;
    float recenterSpeed = 90.0f;     // degrees per second
    // Off by default: recentring the yaw helps, while recentring the pitch
    // undoes the player's own adjustment moments after they make it.
    bool recenterPitchToo = false;
    float recenterPitch = 0.0f;      // pitch the camera settles to, when it does
    // Only swing behind a target that is actually moving; 0 = always.
    float recenterMinSpeed = 0.0f;
    // Film the direction of travel rather than the target's facing — what a
    // character that strafes or slides wants.
    bool recenterOnVelocity = false;

    // Speed response: the field of view opens as the target goes faster, which
    // reads as acceleration without moving the rig. 0 disables it.
    float fovAtRest = 0.0f;          // 0 = leave the camera's own fov alone
    float fovAtSpeed = 0.0f;
    float fovSpeedReference = 10.0f; // target speed at which fovAtSpeed is reached
    float fovDamping = 4.0f;

    // Wall handling.
    float collisionMargin = 0.25f;  // keep the camera this far off the hit surface
    float minDistance = 0.6f;       // never closer than this to the pivot

    SAIDA_REFLECT_BEHAVIOUR(CameraFollowBehaviour, "CameraFollow")

    // ----------------------------------------------------------- Imperative
    // Orbit by an explicit delta in degrees, exactly as mouse input would.
    void orbit(float yawDegrees, float pitchDegrees);
    void setYaw(float yawDegrees);
    void setPitch(float pitchDegrees);
    float yaw() const { return yaw_; }
    float pitch() const { return pitch_; }
    // Start recentring now, without waiting for recenterDelay.
    void recenter();
    // Jump the rig to where it belongs instead of easing there — for a scene
    // change or a teleport, which are the two things that expose smoothing.
    void snap();

protected:
    // Orbit input for this frame, in degrees. The default sums the mouse and
    // the right stick; override to drive the camera from a cutscene.
    virtual glm::vec2 readOrbitInput(float dt);

private:
    glm::vec3 computeDesired(const glm::vec3& pivot) const;

    float yaw_ = -90.0f;   // degrees (orbit angle around the target)
    float pitch_ = 15.0f;
    glm::vec3 camPos_{0.0f};
    glm::vec3 lookAheadOffset_{0.0f};
    glm::vec3 lastTargetPos_{0.0f};
    float pivotHeight_ = 0.0f;
    float idleTimer_ = 0.0f;
    float fov_ = 0.0f;
    bool initialized_ = false;
    bool recentring_ = false;
};

} // namespace saida
