#pragma once

#include "scene/Behaviour.hpp"
#include "core/Reflection.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace saida {

class Animator;
class CharacterBodyNode;

// Third-person character controller with a tunable feel. Three ways in:
// the reflected parameters below, the imperative API (also bound to
// JavaScript), or a subclass overriding the read*/on* hooks.
//
// Every parameter defaults to the behaviour this controller had before it grew
// them, so an existing scene keeps its exact feel and opts into what it names.
// Units are SI: metres, seconds, degrees.
class CharacterBehaviour : public Behaviour {
public:
    CharacterBehaviour() = default;
    ~CharacterBehaviour() override = default;

    enum class TurnMode {
        Smoothed = 0,  // exponential approach at turnSpeed (historic behaviour)
        Constant = 1,  // fixed turnDegreesPerSecond, no easing
        Instant = 2,   // snap to the wanted direction
    };

    void onReady() override;
    void onUpdate(float dt) override;

    SAIDA_REFLECT_BEHAVIOUR(CharacterBehaviour, "Character")

    // ---------------------------------------------------------------- Movement
    float moveSpeed = 5.0f;         // walking speed
    float sprintMultiplier = 1.8f;  // speed factor while Sprint is held
    float groundAcceleration = 0.0f;  // m/s^2; 0 = snap to the wanted velocity
    float groundDeceleration = 0.0f;  // 0 = stop instantly when input is released

    // ------------------------------------------------------------------- Air
    float gravity = 9.81f;
    float airAcceleration = 0.0f;   // 0 = instant, as on the ground
    float airControl = 1.0f;        // 0..1 factor on airborne steering authority
    float fallGravityMultiplier = 1.0f;  // >1 falls faster than it rises
    float maxFallSpeed = 0.0f;      // terminal velocity; 0 = uncapped
    // Hang time at the top of a jump. apexThreshold 0 disables the window.
    float apexGravityMultiplier = 1.0f;
    float apexThreshold = 0.0f;     // |vertical speed| under which it applies

    // ------------------------------------------------------------------ Jump
    float jumpForce = 5.0f;   // take-off speed in m/s
    // When positive this wins over jumpForce, at sqrt(2 * gravity * height) —
    // exact while rising at plain `gravity`, approximate once the apex and fall
    // multipliers bend the arc.
    float jumpHeight = 0.0f;
    // Vertical speed kept when Jump is released while rising; 1 = fixed height.
    float jumpCutoffMultiplier = 1.0f;
    float coyoteTime = 0.0f;      // grace after leaving the ground
    float jumpBufferTime = 0.0f;  // grace for a press made just before landing
    int jumpCount = 1;            // total jumps per airborne trip (2 = double jump)

    // Successive jumps that grow while the chain is alive; 1 disables it.
    int jumpChainCount = 1;
    float jumpChainWindow = 0.0f;      // time after landing where the chain survives
    float jumpChainMultiplier = 1.0f;  // take-off speed factor per chain step
    float jumpChainMinSpeed = 0.0f;    // the chain only builds above this speed

    // ---------------------------------------------------------------- Turning
    bool faceMovement = true;
    TurnMode turnMode = TurnMode::Smoothed;
    float turnSpeed = 12.0f;              // Smoothed: exponential rate
    float turnDegreesPerSecond = 720.0f;  // Constant: fixed angular speed
    float instantTurnBelowSpeed = 0.0f;   // snap instead of easing under this speed

    // Alignment below which reversing brakes instead of arcing. Below -1 it
    // never triggers, a dot product not going there — which is the default.
    float turnAroundDot = -2.0f;
    float turnAroundDeceleration = 0.0f;  // extra braking m/s^2 while reversing
    float turnAroundMinSpeed = 0.0f;      // no skid below this speed

    // ---------------------------------------------------------------- Ground
    // Push off ground steeper than the body's maxSlopeAngle; 0 = inert.
    float slopeSlideAcceleration = 0.0f;

    // ------------------------------------------------------------- Animation
    std::string idleClip = "Idle";
    std::string walkClip = "Walk";
    std::string jumpClip = "Jump";
    // Optional .sgraph (project-relative); the graph then owns playback and
    // this behaviour only feeds its parameters.
    std::string graph;

    // ------------------------------------------------------------------ Input
    // False: read no device at all and move only where setMoveInput and
    // requestJump put it — the mode a script driver uses.
    bool readsInput = true;

    // =================================================================
    // Imperative API — usable from C++, from a subclass, and from scripts.
    // =================================================================

    // Per-frame intent, in camera space: x = right, y = forward, clamped to 1.
    // Cleared after each update, so a driver sets it every frame like a stick.
    void setMoveInput(const glm::vec2& input);
    glm::vec2 moveInput() const { return moveInput_; }
    // Also per-frame. A driver with `readsInput` off must supply it: the only
    // other source is the key the controller is no longer reading.
    void setSprinting(bool sprinting);
    bool isSprinting() const { return sprinting_; }
    // The same intent resolved against the active camera, in world space:
    // what a move aimed "where the player is pointing" needs.
    glm::vec3 wantedDirection() const;

    // Jump through the full ruleset (buffer, coyote, chain, jump count). False
    // when the press only landed in the buffer; it may still fire later.
    bool requestJump();
    // Jump is no longer held: applies jumpCutoffMultiplier if still rising.
    void releaseJump();
    // Explicit take-off, bypassing every rule above: what a special move —
    // a dive, a wall kick — is built from.
    void jumpToHeight(float height);
    void launch(const glm::vec3& velocity);
    void addImpulse(const glm::vec3& impulse);

    void setVelocity(const glm::vec3& velocity);
    glm::vec3 velocity() const;
    glm::vec3 planarVelocity() const;
    float planarSpeed() const;

    // Face a world direction; `instant` skips the configured easing.
    void faceDirection(const glm::vec3& direction, bool instant = false);
    float facingYawDegrees() const;

    // Suspend the solver: the caller owns velocity and facing until it resumes.
    void setSolverEnabled(bool enabled) { solverEnabled_ = enabled; }
    bool solverEnabled() const { return solverEnabled_; }

    // State.
    bool isGrounded() const;
    bool isJumping() const { return jumping_; }
    bool isSkidding() const { return skidding_; }
    float airTime() const { return airTime_; }
    int jumpsUsed() const { return jumpsUsed_; }
    int jumpChainIndex() const { return chainIndex_; }
    void resetJumpChain();

    CharacterBodyNode* body() const;

protected:
    // ------------------------------------------------- Hooks for a subclass
    // What the character is being asked to do. The defaults read the engine
    // actions ("MoveForward"/"Jump"/"Sprint"); override to drive the same
    // solver from an AI, a replay or a different binding set.
    virtual glm::vec2 readMoveInput();
    virtual bool readJumpPressed();
    virtual bool readJumpHeld();
    virtual bool readSprintHeld();

    // What happened. `chainIndex` counts from 0 within a jump chain, and
    // `fromAir` is true for a jump that did not start on the ground.
    virtual void onJumped(int chainIndex, bool fromAir) { (void)chainIndex; (void)fromAir; }
    virtual void onLanded(float impactSpeed) { (void)impactSpeed; }
    virtual void onStartedFalling() {}
    virtual void onTurnAround() {}

    // Animation parameters pushed each frame; override to feed extra ones.
    virtual void updateAnimation(bool onFloor, bool moving);

private:
    float takeOffSpeed(int chainIndex) const;
    void applyJump(float speed, int chainIndex, bool fromAir);
    void updateFacing(const glm::vec3& wanted, float dt);
    bool applyGraph();

    // Per-frame intent.
    glm::vec2 moveInput_{0.0f};
    bool moveInputSet_ = false;
    bool sprinting_ = false;
    bool sprintSet_ = false;
    bool solverEnabled_ = true;

    // Timers and latches.
    float coyoteTimer_ = 0.0f;
    float bufferTimer_ = 0.0f;
    float chainTimer_ = 0.0f;
    float airTime_ = 0.0f;
    int chainIndex_ = 0;
    int jumpsUsed_ = 0;
    bool jumping_ = false;
    bool skidding_ = false;
    bool wasOnFloor_ = true;
    bool jumpHeldLast_ = false;
    // One jump per contact with the ground. Timers alone are not enough: on a
    // slope the controller can keep reporting a floor while the character is
    // already on the way up, and every ground-gated rule then hands out another
    // jump. This latch is structural — only resting on something re-arms it.
    bool groundedSinceJump_ = true;
    float facingYaw_ = 0.0f;
    bool facingInitialized_ = false;

    bool warned_ = false;
    std::vector<Animator*> animators_;
    bool animatorsSearched_ = false;
    uint64_t graphAssetId_ = 0;
    bool graphApplied_ = false;
    bool graphFailed_ = false;
};

} // namespace saida
