#include "behaviours/CharacterBehaviour.hpp"
#include "scene/Node.hpp"
#include "scene/SceneTree.hpp"
#include "physics/CharacterBodyNode.hpp"
#include "scene/animation/AnimGraphAsset.hpp"
#include "scene/animation/Animator.hpp"
#include "graphics/ResourceManager.hpp"
#include "core/Input.hpp"
#include "core/Log.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>

namespace saida {

namespace {
constexpr glm::vec3 kWorldUp{0.0f, 1.0f, 0.0f};

// Speed under which the character counts as standing still.
constexpr float kMovingEpsilon = 0.1f;

// Horizontal forward/right of the active camera (found by group), so movement is
// camera-relative. Falls back to world axes when no camera is present.
void cameraBasis(SceneTree* tree, glm::vec3& forward, glm::vec3& right) {
    forward = glm::vec3(0.0f, 0.0f, -1.0f);
    right = glm::vec3(1.0f, 0.0f, 0.0f);
    if (!tree) return;
    Node* cam = tree->firstInGroup("camera");
    if (!cam) return;
    glm::vec3 camFwd = glm::vec3(cam->worldTransform() * glm::vec4(0.0f, 0.0f, -1.0f, 0.0f));
    camFwd.y = 0.0f;
    if (glm::dot(camFwd, camFwd) < 1e-6f) return;  // camera looking straight up/down
    forward = glm::normalize(camFwd);
    right = glm::normalize(glm::cross(forward, kWorldUp));
}

// Move `current` toward `target` at `rate` m/s^2. A rate of zero means the
// parameter is unset, and the historic behaviour — snap — is what happens.
glm::vec2 approach(glm::vec2 current, glm::vec2 target, float rate, float dt) {
    if (rate <= 0.0f) return target;
    const glm::vec2 delta = target - current;
    const float distance = glm::length(delta);
    if (distance <= 1e-6f) return target;
    const float step = rate * dt;
    if (step >= distance) return target;
    return current + delta * (step / distance);
}

glm::quat facingQuat(const glm::vec3& flatDir) {
    return glm::quat_cast(glm::inverse(glm::lookAt(glm::vec3(0.0f), flatDir, kWorldUp)));
}
}  // namespace

void CharacterBehaviour::onReady() {
    // Default WASD/ZQSD bindings (GLFW key codes are physical QWERTY positions, so
    // these positions already cover ZQSD on an AZERTY layout). Arrow keys too.
    // Bindings are additive (max strength wins), so multiple keys per action work.
    Input::bindKey("MoveForward", KeyCode::W);
    Input::bindKey("MoveLeft", KeyCode::A);
    Input::bindKey("MoveBackward", KeyCode::S);
    Input::bindKey("MoveRight", KeyCode::D);
    Input::bindKey("MoveForward", KeyCode::Up);
    Input::bindKey("MoveLeft", KeyCode::Left);
    Input::bindKey("MoveBackward", KeyCode::Down);
    Input::bindKey("MoveRight", KeyCode::Right);
    Input::bindKey("Jump", KeyCode::Space);
    Input::bindKey("Sprint", KeyCode::LeftShift);
}

CharacterBodyNode* CharacterBehaviour::body() const {
    return node() ? node()->asCharacterBody() : nullptr;
}

bool CharacterBehaviour::isGrounded() const {
    CharacterBodyNode* b = body();
    return b && b->isOnFloor();
}

glm::vec3 CharacterBehaviour::velocity() const {
    CharacterBodyNode* b = body();
    return b ? b->velocity : glm::vec3(0.0f);
}

glm::vec3 CharacterBehaviour::planarVelocity() const {
    const glm::vec3 v = velocity();
    return glm::vec3(v.x, 0.0f, v.z);
}

float CharacterBehaviour::planarSpeed() const {
    return glm::length(planarVelocity());
}

void CharacterBehaviour::setVelocity(const glm::vec3& v) {
    if (CharacterBodyNode* b = body()) b->velocity = v;
}

void CharacterBehaviour::setMoveInput(const glm::vec2& input) {
    moveInput_ = glm::length(input) > 1.0f ? glm::normalize(input) : input;
    moveInputSet_ = true;
}

void CharacterBehaviour::setSprinting(bool sprinting) {
    sprinting_ = sprinting;
    sprintSet_ = true;
}

void CharacterBehaviour::addImpulse(const glm::vec3& impulse) {
    setVelocity(velocity() + impulse);
}

void CharacterBehaviour::launch(const glm::vec3& v) {
    setVelocity(v);
    if (v.y > 0.0f) {
        jumping_ = true;
        groundedSinceJump_ = false;
    }
}

float CharacterBehaviour::takeOffSpeed(int chainIndex) const {
    float base = jumpForce;
    if (jumpHeight > 0.0f && gravity > 0.0f)
        base = std::sqrt(2.0f * gravity * jumpHeight);
    if (chainIndex > 0 && jumpChainMultiplier != 1.0f)
        base *= std::pow(jumpChainMultiplier, static_cast<float>(chainIndex));
    return base;
}

void CharacterBehaviour::applyJump(float speed, int chainIndex, bool fromAir) {
    glm::vec3 v = velocity();
    v.y = speed;
    setVelocity(v);
    jumping_ = true;
    groundedSinceJump_ = false;
    bufferTimer_ = 0.0f;
    coyoteTimer_ = 0.0f;
    chainTimer_ = 0.0f;
    ++jumpsUsed_;
    onJumped(chainIndex, fromAir);
}

void CharacterBehaviour::jumpToHeight(float height) {
    if (height <= 0.0f || gravity <= 0.0f) return;
    applyJump(std::sqrt(2.0f * gravity * height), 0, !isGrounded());
}

bool CharacterBehaviour::requestJump() {
    CharacterBodyNode* b = body();
    if (!b) return false;

    const bool onFloor = b->isOnFloor();
    const bool grounded = onFloor || coyoteTimer_ > 0.0f;

    // The latch matters: the controller can still report a floor for a frame
    // after take-off, and a buffered press would then fire again in mid-air.
    if (grounded && groundedSinceJump_ && !b->isOnSteepSlope()) {
        const bool chaining = chainTimer_ > 0.0f && jumpChainCount > 1 &&
                              planarSpeed() > jumpChainMinSpeed;
        chainIndex_ = chaining ? std::min(chainIndex_ + 1, jumpChainCount - 1) : 0;
        jumpsUsed_ = 0;
        applyJump(takeOffSpeed(chainIndex_), chainIndex_, false);
        return true;
    }

    // Air jumps (double jump and beyond) never chain: the chain is a reward for
    // rhythm on the ground, not for holding the button down.
    if (!onFloor && jumpsUsed_ < jumpCount && jumpsUsed_ > 0) {
        chainIndex_ = 0;
        applyJump(takeOffSpeed(0), 0, true);
        return true;
    }

    bufferTimer_ = jumpBufferTime;
    return false;
}

void CharacterBehaviour::releaseJump() {
    if (!jumping_ || jumpCutoffMultiplier >= 1.0f) return;
    glm::vec3 v = velocity();
    if (v.y <= 0.0f) return;  // already falling: nothing left to cut
    v.y *= jumpCutoffMultiplier;
    setVelocity(v);
}

void CharacterBehaviour::resetJumpChain() {
    chainIndex_ = 0;
    chainTimer_ = 0.0f;
}

void CharacterBehaviour::faceDirection(const glm::vec3& direction, bool instant) {
    if (!node()) return;
    glm::vec3 flat(direction.x, 0.0f, direction.z);
    if (glm::dot(flat, flat) < 1e-8f) return;
    flat = glm::normalize(flat);
    const glm::quat target = facingQuat(flat);
    if (instant || turnMode == TurnMode::Instant) {
        node()->transform().rotation = target;
        return;
    }
    node()->transform().rotation = target;  // no dt here; easing belongs to the update
}

float CharacterBehaviour::facingYawDegrees() const {
    if (!node()) return 0.0f;
    const glm::quat r = node()->transform().rotation;
    return glm::degrees(std::atan2(2.0f * (r.w * r.y + r.x * r.z),
                                   1.0f - 2.0f * (r.y * r.y + r.x * r.x)));
}

glm::vec2 CharacterBehaviour::readMoveInput() {
    return Input::getVector("MoveLeft", "MoveRight", "MoveBackward", "MoveForward");
}

bool CharacterBehaviour::readJumpPressed() { return Input::isActionJustPressed("Jump"); }
bool CharacterBehaviour::readJumpHeld() { return Input::isActionHeld("Jump"); }
bool CharacterBehaviour::readSprintHeld() { return Input::isActionHeld("Sprint"); }

glm::vec3 CharacterBehaviour::wantedDirection() const {
    glm::vec3 forward, right;
    cameraBasis(tree(), forward, right);
    glm::vec3 dir = right * moveInput_.x + forward * moveInput_.y;
    if (glm::length(dir) > 1.0f) dir = glm::normalize(dir);  // no faster diagonals
    return dir;
}

void CharacterBehaviour::updateFacing(const glm::vec3& wanted, float dt) {
    if (!faceMovement || !node()) return;
    if (glm::dot(wanted, wanted) < 1e-8f) return;
    const glm::vec3 flatDir = glm::normalize(glm::vec3(wanted.x, 0.0f, wanted.z));
    const glm::quat target = facingQuat(flatDir);
    const glm::quat current = node()->transform().rotation;

    if (turnMode == TurnMode::Instant ||
        (instantTurnBelowSpeed > 0.0f && planarSpeed() < instantTurnBelowSpeed)) {
        node()->transform().rotation = target;
        return;
    }
    if (turnMode == TurnMode::Constant) {
        // Angle between the two orientations, then a step capped in degrees.
        const float cosHalf = std::min(1.0f, std::abs(glm::dot(current, target)));
        const float angle = glm::degrees(2.0f * std::acos(cosHalf));
        if (angle <= 1e-3f) return;
        const float step = turnDegreesPerSecond * dt;
        const float t = step >= angle ? 1.0f : step / angle;
        node()->transform().rotation = glm::normalize(glm::slerp(current, target, t));
        return;
    }
    const float a = 1.0f - std::exp(-turnSpeed * dt);
    node()->transform().rotation = glm::normalize(glm::slerp(current, target, a));
}

void CharacterBehaviour::onUpdate(float dt) {
    CharacterBodyNode* b = body();
    if (!b) {
        if (!warned_) {
            Log::warn("CharacterBehaviour must be on a CharacterBody node — ignored");
            warned_ = true;
        }
        return;
    }
    if (dt <= 0.0f) return;

    const bool onFloor = b->isOnFloor();
    const glm::vec3 entryVelocity = b->velocity;

    // --- timers -------------------------------------------------------------
    coyoteTimer_ = onFloor ? coyoteTime : std::max(0.0f, coyoteTimer_ - dt);
    bufferTimer_ = std::max(0.0f, bufferTimer_ - dt);
    chainTimer_ = std::max(0.0f, chainTimer_ - dt);
    airTime_ = onFloor ? 0.0f : airTime_ + dt;

    // --- landing and take-off transitions -----------------------------------
    if (onFloor && !wasOnFloor_) {
        // The chain survives a landing that kept momentum, and only then.
        const float speed = glm::length(glm::vec2(entryVelocity.x, entryVelocity.z));
        chainTimer_ = (jumpChainCount > 1 && speed > jumpChainMinSpeed) ? jumpChainWindow : 0.0f;
        if (chainTimer_ <= 0.0f) chainIndex_ = 0;
        jumpsUsed_ = 0;
        jumping_ = false;
        onLanded(std::max(0.0f, -entryVelocity.y));
    } else if (!onFloor && wasOnFloor_) {
        onStartedFalling();
    }
    wasOnFloor_ = onFloor;
    if (onFloor && entryVelocity.y <= 0.01f) groundedSinceJump_ = true;

    // --- intent -------------------------------------------------------------
    if (!moveInputSet_) moveInput_ = readsInput ? readMoveInput() : glm::vec2(0.0f);
    if (glm::length(moveInput_) > 1.0f) moveInput_ = glm::normalize(moveInput_);

    const bool jumpPressed = readsInput && readJumpPressed();
    const bool jumpHeld = readsInput ? readJumpHeld() : jumpHeldLast_;
    if (!sprintSet_) sprinting_ = readsInput && readSprintHeld();
    const bool sprinting = sprinting_;

    if (!solverEnabled_) {
        // The caller owns the body this frame. State stays coherent so the
        // frame after it hands control back behaves normally.
        moveInputSet_ = false;
        sprintSet_ = false;
        jumpHeldLast_ = jumpHeld;
        updateAnimation(onFloor, glm::length(glm::vec2(entryVelocity.x, entryVelocity.z)) > kMovingEpsilon);
        return;
    }

    if (jumpPressed) {
        bufferTimer_ = jumpBufferTime;
        requestJump();
    } else if (bufferTimer_ > 0.0f) {
        requestJump();  // a press that arrived early, retried until it expires
    }
    if (readsInput && jumpHeldLast_ && !jumpHeld) releaseJump();
    jumpHeldLast_ = jumpHeld;

    // --- horizontal movement -------------------------------------------------
    const glm::vec3 wanted = wantedDirection();
    const float speed = moveSpeed * (sprinting ? sprintMultiplier : 1.0f);
    glm::vec3 v = b->velocity;
    glm::vec2 planar(v.x, v.z);
    const glm::vec2 target(wanted.x * speed, wanted.z * speed);
    const bool hasInput = glm::dot(wanted, wanted) > 1e-8f;

    // Turning around: opposing the current heading brakes instead of arcing.
    const float currentSpeed = glm::length(planar);
    bool skid = false;
    if (hasInput && turnAroundDot >= -1.0f && currentSpeed > turnAroundMinSpeed &&
        currentSpeed > 1e-4f) {
        const float alignment = glm::dot(glm::normalize(planar),
                                         glm::normalize(glm::vec2(wanted.x, wanted.z)));
        skid = alignment < turnAroundDot;
    }
    if (skid && !skidding_) onTurnAround();
    skidding_ = skid;

    if (skid && turnAroundDeceleration > 0.0f) {
        const float drop = std::min(currentSpeed, turnAroundDeceleration * dt);
        planar -= glm::normalize(planar) * drop;
    } else if (hasInput) {
        // A launch, a slope or a moving platform can leave more speed than the
        // tuning asked for. Snapping it down deletes every move built on
        // `launch`, so the excess bleeds off at the braking rate instead.
        const bool overspeed = currentSpeed > glm::length(target) + 1e-4f;
        float rate = onFloor ? (overspeed ? groundDeceleration : groundAcceleration)
                             : airAcceleration * std::max(0.0f, airControl);
        planar = approach(planar, target, rate, dt);
    } else {
        const float rate = onFloor ? groundDeceleration : airAcceleration;
        planar = approach(planar, glm::vec2(0.0f), rate, dt);
    }
    v.x = planar.x;
    v.z = planar.y;

    // --- vertical ------------------------------------------------------------
    if (onFloor && !jumping_) {
        if (v.y < 0.0f) v.y = 0.0f;  // stay glued instead of accumulating fall speed
    } else {
        float scale = 1.0f;
        if (v.y < 0.0f) scale *= fallGravityMultiplier;
        if (apexThreshold > 0.0f && std::abs(v.y) < apexThreshold) scale *= apexGravityMultiplier;
        v.y -= gravity * scale * dt;
        if (maxFallSpeed > 0.0f) v.y = std::max(v.y, -maxFallSpeed);
    }

    // A steep slope is not standable: push the character off it.
    if (slopeSlideAcceleration > 0.0f && b->isOnSteepSlope()) {
        const glm::vec3 n = b->groundNormal();
        v.x += n.x * slopeSlideAcceleration * dt;
        v.z += n.z * slopeSlideAcceleration * dt;
    }

    b->velocity = v;

    const bool moving = glm::length(glm::vec2(v.x, v.z)) > kMovingEpsilon;
    // A skid faces where the stick points, not where the body still slides.
    updateFacing(hasInput ? wanted : glm::vec3(0.0f), dt);

    moveInputSet_ = false;
    sprintSet_ = false;
    updateAnimation(onFloor, moving);
}

bool CharacterBehaviour::applyGraph() {
    ResourceManager& resources = tree()->resources();
    if (graphAssetId_ == kAssetInvalid) {
        graphAssetId_ = resources.loadAnimGraph(tree()->resolveProjectPath(graph));
        if (graphAssetId_ == kAssetInvalid) {
            graphFailed_ = true;
            Log::warn("Character: cannot request anim graph '", graph, "'");
            return false;
        }
    }

    const AssetLoadState state = resources.animGraphLoadState(graphAssetId_);
    if (state == AssetLoadState::Queued || state == AssetLoadState::Loading) return false;
    if (state == AssetLoadState::Failed) {
        graphFailed_ = true;
        const std::string error = resources.animGraphLoadError(graphAssetId_);
        Log::warn("Character: cannot apply anim graph '", graph, "'",
                  error.empty() ? "" : (": " + error));
        return false;
    }

    const AnimGraphAsset* loaded = resources.getAnimGraph(graphAssetId_);
    if (!loaded) {
        graphFailed_ = true;
        Log::warn("Character: cannot apply anim graph '", graph, "'");
        return false;
    }
    for (Animator* a : animators_) {
        std::vector<AssetDiagnostic> diags;
        if (!a->setGraph(*loaded, &diags)) {
            graphFailed_ = true;
            Log::warn("Character: cannot apply anim graph '", graph, "'",
                      diags.empty() ? "" : (": " + diags.front().message));
            return false;
        }
    }
    graphApplied_ = true;
    return true;
}

void CharacterBehaviour::updateAnimation(bool onFloor, bool moving) {
    if (!animatorsSearched_) {
        node()->findBehavioursInChildren<Animator>(animators_);
        animatorsSearched_ = true;
    }
    if (animators_.empty()) return;  // no skinned character → nothing to drive

    if (!graph.empty() && !graphFailed_ && !graphApplied_ && !applyGraph()) return;

    // Parameters are always fed: whether the graph came from this behaviour or
    // from an AnimGraph behaviour beside it, a graph that reads "speed" wants it.
    const glm::vec3 v = velocity();
    bool anyAuthoredGraph = false;
    for (Animator* a : animators_) {
        if (!a->hasAuthoredGraph()) continue;
        anyAuthoredGraph = true;
        a->setFloat("speed", glm::length(glm::vec2(v.x, v.z)));
        a->setFloat("vspeed", v.y);
        a->setBool("airborne", !onFloor);
        a->setBool("skidding", skidding_);
    }
    // A graph owns its own transitions: naming a clip here would tear them
    // apart, so the clip fallback only applies to animators without one.
    if (anyAuthoredGraph) return;

    const std::string& want = !onFloor ? jumpClip : (moving ? walkClip : idleClip);
    if (want.empty()) return;
    for (Animator* a : animators_)
        if (!a->hasAuthoredGraph() && a->clips().count(want))
            a->play(want);  // play() no-ops if it's already the current clip
}

void CharacterBehaviour::describe(reflect::TypeBuilder<CharacterBehaviour>& t) {
    t.doc("Third-person character controller with a tunable feel: camera-relative "
          "movement, acceleration, a jump with coyote time, buffering, variable "
          "height and chaining, and turn/turn-around control. Attach to a "
          "CharacterBody node. Every parameter defaults to the behaviour this "
          "controller had before it grew them, so adding one is always opt-in.");

    t.property("moveSpeed", &CharacterBehaviour::moveSpeed).range(0.0, 50.0)
        .group("Movement").tooltip("walking speed in m/s");
    t.property("sprintMultiplier", &CharacterBehaviour::sprintMultiplier).range(1.0, 5.0)
        .group("Movement").tooltip("speed factor while holding Sprint");
    t.property("groundAcceleration", &CharacterBehaviour::groundAcceleration).range(0.0, 200.0)
        .group("Movement").tooltip("m/s^2 toward the wanted velocity; 0 = snap instantly");
    t.property("groundDeceleration", &CharacterBehaviour::groundDeceleration).range(0.0, 200.0)
        .group("Movement").tooltip("m/s^2 of braking with no input; 0 = stop instantly");
    t.property("readsInput", &CharacterBehaviour::readsInput)
        .group("Movement").tooltip("off = the controller only moves where setMoveInput/requestJump put it");

    t.property("gravity", &CharacterBehaviour::gravity).range(0.0, 100.0).group("Air");
    t.property("airAcceleration", &CharacterBehaviour::airAcceleration).range(0.0, 200.0)
        .group("Air").tooltip("m/s^2 while airborne; 0 = snap instantly");
    t.property("airControl", &CharacterBehaviour::airControl).range(0.0, 1.0)
        .group("Air").tooltip("steering authority in the air, 0 = none");
    t.property("fallGravityMultiplier", &CharacterBehaviour::fallGravityMultiplier).range(0.1, 5.0)
        .group("Air").tooltip("gravity factor while falling; >1 makes the jump snappy");
    t.property("maxFallSpeed", &CharacterBehaviour::maxFallSpeed).range(0.0, 100.0)
        .group("Air").tooltip("terminal velocity in m/s; 0 = uncapped");
    t.property("apexGravityMultiplier", &CharacterBehaviour::apexGravityMultiplier).range(0.0, 2.0)
        .group("Air").tooltip("gravity factor near the top of a jump; <1 adds hang time");
    t.property("apexThreshold", &CharacterBehaviour::apexThreshold).range(0.0, 10.0)
        .group("Air").tooltip("vertical speed under which the apex factor applies; 0 = disabled");

    t.property("jumpForce", &CharacterBehaviour::jumpForce).range(0.0, 30.0)
        .group("Jump").tooltip("take-off speed in m/s");
    t.property("jumpHeight", &CharacterBehaviour::jumpHeight).range(0.0, 20.0)
        .group("Jump").tooltip("target height in metres; when >0 it wins over jumpForce");
    t.property("jumpCutoffMultiplier", &CharacterBehaviour::jumpCutoffMultiplier).range(0.0, 1.0)
        .group("Jump").tooltip("vertical speed kept when Jump is released while rising; 1 = fixed-height jump");
    t.property("coyoteTime", &CharacterBehaviour::coyoteTime).range(0.0, 1.0)
        .group("Jump").tooltip("seconds of grace after walking off an edge");
    t.property("jumpBufferTime", &CharacterBehaviour::jumpBufferTime).range(0.0, 1.0)
        .group("Jump").tooltip("seconds a press survives while still airborne");
    t.property("jumpCount", &CharacterBehaviour::jumpCount).range(1.0, 6.0)
        .group("Jump").tooltip("jumps per airborne trip; 2 = double jump");
    t.property("jumpChainCount", &CharacterBehaviour::jumpChainCount).range(1.0, 6.0)
        .group("Jump").tooltip("successive jumps that grow; 1 = no chain");
    t.property("jumpChainWindow", &CharacterBehaviour::jumpChainWindow).range(0.0, 2.0)
        .group("Jump").tooltip("seconds after landing where the chain survives");
    t.property("jumpChainMultiplier", &CharacterBehaviour::jumpChainMultiplier).range(1.0, 2.0)
        .group("Jump").tooltip("take-off speed factor per chain step");
    t.property("jumpChainMinSpeed", &CharacterBehaviour::jumpChainMinSpeed).range(0.0, 20.0)
        .group("Jump").tooltip("the chain only builds above this speed");

    t.property("faceMovement", &CharacterBehaviour::faceMovement)
        .group("Turning").tooltip("turn to face the movement direction");
    t.property("turnMode", &CharacterBehaviour::turnMode)
        .group("Turning").enumValues({"Smoothed", "Constant", "Instant"});
    t.property("turnSpeed", &CharacterBehaviour::turnSpeed).range(0.0, 50.0)
        .group("Turning").tooltip("Smoothed mode: exponential rate");
    t.property("turnDegreesPerSecond", &CharacterBehaviour::turnDegreesPerSecond).range(0.0, 3600.0)
        .group("Turning").tooltip("Constant mode: fixed angular speed");
    t.property("instantTurnBelowSpeed", &CharacterBehaviour::instantTurnBelowSpeed).range(0.0, 20.0)
        .group("Turning").tooltip("snap instead of easing under this speed; 0 = always ease");
    t.property("turnAroundDot", &CharacterBehaviour::turnAroundDot).range(-2.0, 1.0)
        .group("Turning").tooltip("alignment below which a reversal brakes; below -1 disables it");
    t.property("turnAroundDeceleration", &CharacterBehaviour::turnAroundDeceleration).range(0.0, 200.0)
        .group("Turning").tooltip("extra braking m/s^2 while turning around");
    t.property("turnAroundMinSpeed", &CharacterBehaviour::turnAroundMinSpeed).range(0.0, 20.0)
        .group("Turning").tooltip("no skid below this speed");

    t.property("slopeSlideAcceleration", &CharacterBehaviour::slopeSlideAcceleration).range(0.0, 100.0)
        .group("Ground").tooltip("push off ground steeper than the body's maxSlopeAngle; 0 = inert");

    t.property("idleClip", &CharacterBehaviour::idleClip).group("Animation").tooltip("animation clip name");
    t.property("walkClip", &CharacterBehaviour::walkClip).group("Animation").tooltip("animation clip name");
    t.property("jumpClip", &CharacterBehaviour::jumpClip).group("Animation").tooltip("animation clip name");
    t.property("graph", &CharacterBehaviour::graph).group("Animation")
        .tooltip(".sgraph path (project-relative); when set it drives speed/vspeed/airborne/skidding");
}

} // namespace saida
