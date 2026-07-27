// Every tunable of CharacterBehaviour, proved headlessly: the controller is
// driven through its imperative API with `readsInput` off, which is also the
// mode a script driver uses.
#include "behaviours/CharacterBehaviour.hpp"
#include "physics/CharacterBodyNode.hpp"
#include "physics/CollisionShapeNode.hpp"
#include "physics/StaticBodyNode.hpp"
#include "scene/Scene.hpp"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>

using namespace saida;

namespace {

int gChecks = 0;

void check(bool ok, const std::string& what) {
    ++gChecks;
    if (!ok) {
        std::fprintf(stderr, "[character] FAILED: %s\n", what.c_str());
        std::abort();
    }
}

void checkNear(float value, float expected, float tolerance, const std::string& what) {
    if (std::fabs(value - expected) > tolerance)
        std::fprintf(stderr, "[character] %s: got %.4f, expected %.4f +/- %.4f\n",
                     what.c_str(), value, expected, tolerance);
    check(std::fabs(value - expected) <= tolerance, what);
}

constexpr float kDt = 1.0f / 60.0f;

// A floor plus a character resting on it, with the controller attached and its
// input reading disabled.
struct Rig {
    Scene scene;
    CharacterBodyNode* body = nullptr;
    CharacterBehaviour* controller = nullptr;

    explicit Rig(CharacterBehaviour* custom = nullptr) {
        auto* floor = scene.createChild<StaticBodyNode>();
        floor->transform().position = {0.0f, 0.0f, 0.0f};
        auto floorShape = std::make_unique<CollisionShapeNode>();
        floorShape->shapeType = CollisionShapeType::Box;
        floorShape->halfExtents = {40.0f, 0.5f, 40.0f};
        floor->addChild(std::move(floorShape));

        body = scene.createChild<CharacterBodyNode>();
        body->transform().position = {0.0f, 1.6f, 0.0f};
        auto shape = std::make_unique<CollisionShapeNode>();
        shape->shapeType = CollisionShapeType::Capsule;
        shape->radius = 0.4f;
        shape->height = 1.8f;
        shape->axis = 1;
        body->addChild(std::move(shape));

        controller = custom
            ? static_cast<CharacterBehaviour*>(
                  body->addBehaviour(std::unique_ptr<Behaviour>(custom)))
            : body->addBehaviour<CharacterBehaviour>();
        controller->readsInput = false;
        settle();
    }

    void settle() {
        for (int i = 0; i < 120 && !body->isOnFloor(); ++i) scene.update(kDt);
        for (int i = 0; i < 10; ++i) scene.update(kDt);
    }

    void step(int frames, const glm::vec2& input = glm::vec2(0.0f)) {
        for (int i = 0; i < frames; ++i) {
            if (input != glm::vec2(0.0f)) controller->setMoveInput(input);
            scene.update(kDt);
        }
    }

    // Runs until the character has left the ground and come back, returning the
    // highest point reached relative to the starting height.
    float jumpApex(int maxFrames = 400) {
        const float start = body->transform().position.y;
        float peak = start;
        for (int i = 0; i < maxFrames; ++i) {
            scene.update(kDt);
            peak = std::max(peak, body->transform().position.y);
            if (i > 4 && body->isOnFloor() && body->velocity.y <= 0.0f) break;
        }
        return peak - start;
    }
};

// --- defaults reproduce the controller as it was ---------------------------

void testDefaultsAreUnchanged() {
    Rig rig;
    // No acceleration configured: horizontal velocity snaps to the target.
    rig.controller->moveSpeed = 6.0f;
    rig.controller->setMoveInput({0.0f, 1.0f});
    rig.scene.update(kDt);
    checkNear(std::fabs(rig.body->velocity.z), 6.0f, 0.01f,
              "default acceleration snaps to move speed in one frame");

    // Releasing the stick stops the character just as abruptly.
    rig.scene.update(kDt);
    checkNear(rig.controller->planarSpeed(), 0.0f, 0.01f,
              "default deceleration stops in one frame");

    // No coyote time, no buffering, one jump.
    check(rig.controller->requestJump(), "grounded jump fires");
    checkNear(rig.body->velocity.y, rig.controller->jumpForce, 0.01f,
              "take-off speed is jumpForce");
    check(!rig.controller->requestJump(), "a second jump in the air is refused by default");
}

// --- movement --------------------------------------------------------------

void testGroundAcceleration() {
    Rig rig;
    rig.controller->moveSpeed = 10.0f;
    rig.controller->groundAcceleration = 20.0f;  // half a second to top speed
    rig.step(15, {0.0f, 1.0f});                  // a quarter of a second
    const float speed = rig.controller->planarSpeed();
    check(speed > 3.0f && speed < 7.0f, "acceleration ramps rather than snapping");
    rig.step(45, {0.0f, 1.0f});
    checkNear(rig.controller->planarSpeed(), 10.0f, 0.2f, "acceleration reaches top speed");
}

void testGroundDeceleration() {
    Rig rig;
    rig.controller->moveSpeed = 10.0f;
    rig.controller->groundDeceleration = 10.0f;
    rig.step(30, {0.0f, 1.0f});
    const float moving = rig.controller->planarSpeed();
    rig.step(15);  // no input for a quarter second
    const float coasting = rig.controller->planarSpeed();
    check(coasting < moving && coasting > 0.5f, "deceleration brakes without stopping dead");
    rig.step(60);
    checkNear(rig.controller->planarSpeed(), 0.0f, 0.05f, "deceleration comes to rest");
}

// --- jump ------------------------------------------------------------------

void testJumpHeight() {
    Rig rig;
    rig.controller->gravity = 20.0f;
    rig.controller->jumpHeight = 3.0f;
    rig.controller->requestJump();
    checkNear(rig.jumpApex(), 3.0f, 0.25f, "jumpHeight reaches the requested height");
}

void testJumpCutoff() {
    Rig full;
    full.controller->gravity = 20.0f;
    full.controller->jumpHeight = 3.0f;
    full.controller->requestJump();
    const float held = full.jumpApex();

    Rig cut;
    cut.controller->gravity = 20.0f;
    cut.controller->jumpHeight = 3.0f;
    cut.controller->jumpCutoffMultiplier = 0.5f;
    cut.controller->requestJump();
    cut.scene.update(kDt);
    cut.controller->releaseJump();
    const float tapped = cut.jumpApex();

    check(tapped < held * 0.6f, "releasing early cuts the jump short");
    check(tapped > 0.2f, "a cut jump still leaves the ground");
}

void testCoyoteTime() {
    Rig rig;
    rig.controller->coyoteTime = 0.2f;
    // Walk off the edge of the floor: 40 m half-extent, so teleport instead.
    rig.body->transform().position = {0.0f, 6.0f, 0.0f};
    rig.scene.update(kDt);
    check(!rig.body->isOnFloor(), "character is airborne after being lifted");
    // Coyote time was armed while grounded and has not run out yet.
    check(rig.controller->requestJump(), "coyote time still allows a jump just after leaving ground");
}

void testJumpBuffer() {
    Rig rig;
    rig.controller->jumpBufferTime = 0.25f;
    rig.body->transform().position = {0.0f, 4.0f, 0.0f};
    rig.scene.update(kDt);
    rig.controller->coyoteTime = 0.0f;
    for (int i = 0; i < 6; ++i) rig.scene.update(kDt);  // fall, no coyote left

    check(!rig.controller->requestJump(), "a press in mid-air does not jump");
    // Nothing calls requestJump again: the only thing that can launch the
    // character now is the buffer being retried inside onUpdate.
    bool fired = false;
    for (int i = 0; i < 60 && !fired; ++i) {
        rig.scene.update(kDt);
        if (rig.body->velocity.y > 1.0f) fired = true;
    }
    check(fired, "the buffered press fires by itself on landing");

    // And a press that expires before the ground arrives is forgotten.
    Rig expired;
    expired.controller->jumpBufferTime = 0.05f;
    expired.body->transform().position = {0.0f, 12.0f, 0.0f};
    expired.scene.update(kDt);
    for (int i = 0; i < 10; ++i) expired.scene.update(kDt);
    expired.controller->requestJump();  // buffered, then left to rot
    bool late = false;
    for (int i = 0; i < 200 && !late; ++i) {
        expired.scene.update(kDt);
        if (expired.body->isOnFloor() && expired.body->velocity.y > 1.0f) late = true;
    }
    check(!late, "an expired buffer does not jump on landing");
}

void testDoubleJump() {
    Rig rig;
    rig.controller->jumpCount = 2;
    check(rig.controller->requestJump(), "first jump fires");
    for (int i = 0; i < 8; ++i) rig.scene.update(kDt);
    check(!rig.body->isOnFloor(), "character is airborne");
    check(rig.controller->requestJump(), "second jump fires in the air");
    check(!rig.controller->requestJump(), "a third jump is refused with jumpCount 2");
}

void testJumpChain() {
    Rig rig;
    rig.controller->gravity = 20.0f;
    rig.controller->jumpForce = 8.0f;
    rig.controller->jumpChainCount = 3;
    rig.controller->jumpChainWindow = 0.4f;
    rig.controller->jumpChainMultiplier = 1.2f;
    rig.controller->moveSpeed = 6.0f;
    rig.controller->jumpChainMinSpeed = 1.0f;

    float previous = 0.0f;
    for (int chain = 0; chain < 3; ++chain) {
        rig.controller->setMoveInput({0.0f, 1.0f});
        rig.scene.update(kDt);  // keep momentum so the chain survives the landing
        check(rig.controller->requestJump(), "chained jump fires");
        check(rig.controller->jumpChainIndex() == chain, "chain index advances");
        // Fly the arc while still holding the stick.
        float peak = rig.body->transform().position.y;
        for (int i = 0; i < 400; ++i) {
            rig.controller->setMoveInput({0.0f, 1.0f});
            rig.scene.update(kDt);
            peak = std::max(peak, rig.body->transform().position.y);
            if (i > 4 && rig.body->isOnFloor()) break;
        }
        const float height = peak - rig.body->transform().position.y;
        if (chain > 0) check(height > previous, "each chained jump goes higher");
        previous = height;
    }
}

// --- gravity shaping -------------------------------------------------------

void testFallGravityMultiplier() {
    Rig symmetric;
    symmetric.controller->gravity = 20.0f;
    symmetric.controller->jumpHeight = 3.0f;
    symmetric.controller->requestJump();
    const float symmetricApex = symmetric.jumpApex();

    Rig heavy;
    heavy.controller->gravity = 20.0f;
    heavy.controller->jumpHeight = 3.0f;
    heavy.controller->fallGravityMultiplier = 3.0f;
    heavy.controller->requestJump();
    const float heavyApex = heavy.jumpApex();

    // The rise is untouched, so the apex matches; only the way down changes.
    checkNear(heavyApex, symmetricApex, 0.2f, "fall gravity leaves the rise alone");

    // What must differ is how long the fall takes. Measured from the apex, a
    // triple-gravity descent has to be clearly quicker.
    auto fallFrames = [](float multiplier) {
        Rig rig;
        rig.controller->gravity = 20.0f;
        rig.controller->fallGravityMultiplier = multiplier;
        rig.body->transform().position = {0.0f, 12.0f, 0.0f};
        rig.body->velocity = {0.0f, 0.0f, 0.0f};
        int frames = 0;
        for (; frames < 600; ++frames) {
            rig.scene.update(kDt);
            if (rig.body->isOnFloor()) break;
        }
        return frames;
    };
    const int slow = fallFrames(1.0f);
    const int fast = fallFrames(3.0f);
    check(fast < slow, "a heavier fall reaches the ground sooner");
    check(fast < static_cast<int>(slow * 0.75f), "and noticeably so");
}

void testMaxFallSpeed() {
    Rig rig;
    rig.controller->gravity = 30.0f;
    rig.controller->maxFallSpeed = 8.0f;
    rig.body->transform().position = {0.0f, 30.0f, 0.0f};
    for (int i = 0; i < 90; ++i) rig.scene.update(kDt);
    check(rig.body->velocity.y >= -8.01f, "terminal velocity caps the fall");
    checkNear(rig.body->velocity.y, -8.0f, 0.1f, "terminal velocity is reached");
}

void testAirControl() {
    Rig rig;
    rig.controller->moveSpeed = 10.0f;
    rig.controller->airAcceleration = 40.0f;
    rig.controller->airControl = 0.25f;
    rig.body->transform().position = {0.0f, 20.0f, 0.0f};
    rig.scene.update(kDt);
    rig.step(15, {0.0f, 1.0f});
    const float damped = rig.controller->planarSpeed();

    Rig free;
    free.controller->moveSpeed = 10.0f;
    free.controller->airAcceleration = 40.0f;
    free.controller->airControl = 1.0f;
    free.body->transform().position = {0.0f, 20.0f, 0.0f};
    free.scene.update(kDt);
    free.step(15, {0.0f, 1.0f});

    check(damped < free.controller->planarSpeed() * 0.5f,
          "air control scales steering authority");
}

// --- turning ---------------------------------------------------------------

void testTurnModes() {
    Rig instant;
    instant.controller->turnMode = CharacterBehaviour::TurnMode::Instant;
    instant.controller->setMoveInput({1.0f, 0.0f});
    instant.scene.update(kDt);
    checkNear(std::fabs(instant.controller->facingYawDegrees()), 90.0f, 1.0f,
              "instant turn faces the input immediately");

    Rig smoothed;
    smoothed.controller->turnMode = CharacterBehaviour::TurnMode::Smoothed;
    smoothed.controller->turnSpeed = 6.0f;
    smoothed.controller->setMoveInput({1.0f, 0.0f});
    smoothed.scene.update(kDt);
    check(std::fabs(smoothed.controller->facingYawDegrees()) < 45.0f,
          "smoothed turn only travels part of the way in one frame");

    Rig constant;
    constant.controller->turnMode = CharacterBehaviour::TurnMode::Constant;
    constant.controller->turnDegreesPerSecond = 180.0f;
    constant.controller->setMoveInput({1.0f, 0.0f});
    constant.scene.update(kDt);
    checkNear(std::fabs(constant.controller->facingYawDegrees()), 3.0f, 1.0f,
              "constant turn advances exactly its angular speed");
}

void testTurnAround() {
    Rig rig;
    rig.controller->moveSpeed = 10.0f;
    rig.controller->turnAroundDot = -0.5f;
    rig.controller->turnAroundDeceleration = 40.0f;
    rig.controller->turnAroundMinSpeed = 2.0f;
    rig.step(30, {0.0f, 1.0f});
    check(!rig.controller->isSkidding(), "running forward is not a skid");

    const float before = rig.controller->planarSpeed();
    rig.controller->setMoveInput({0.0f, -1.0f});
    rig.scene.update(kDt);
    check(rig.controller->isSkidding(), "reversing the stick starts a skid");
    check(rig.controller->planarSpeed() < before, "the skid brakes the character");
}

// --- hooks and imperative API ----------------------------------------------

class CountingCharacter : public CharacterBehaviour {
public:
    int jumps = 0;
    int landings = 0;
    int falls = 0;
    int turnArounds = 0;
    int lastChainIndex = -1;
    float lastImpact = 0.0f;

protected:
    void onJumped(int chainIndex, bool fromAir) override {
        ++jumps;
        lastChainIndex = chainIndex;
        (void)fromAir;
    }
    void onLanded(float impactSpeed) override { ++landings; lastImpact = impactSpeed; }
    void onStartedFalling() override { ++falls; }
    void onTurnAround() override { ++turnArounds; }
};

void testSubclassHooks() {
    auto* custom = new CountingCharacter();
    Rig rig(custom);
    rig.controller->gravity = 20.0f;
    rig.controller->jumpForce = 8.0f;

    check(rig.controller->requestJump(), "subclass jumps");
    check(custom->jumps == 1, "onJumped fired");
    check(custom->lastChainIndex == 0, "onJumped reports the chain index");

    rig.jumpApex();
    for (int i = 0; i < 30; ++i) rig.scene.update(kDt);
    check(custom->falls >= 1, "onStartedFalling fired when leaving the ground");
    check(custom->landings >= 1, "onLanded fired on the way back");
    check(custom->lastImpact > 1.0f, "onLanded reports a downward impact speed");
}

void testSolverCanBeSuspended() {
    Rig rig;
    rig.controller->setSolverEnabled(false);
    rig.controller->setVelocity({0.0f, 0.0f, 5.0f});
    rig.scene.update(kDt);
    // With the solver off the controller neither applies gravity nor rewrites
    // the velocity a caller set.
    checkNear(rig.body->velocity.z, 5.0f, 0.01f, "a suspended solver leaves velocity alone");

    rig.controller->setSolverEnabled(true);
    rig.controller->setMoveInput({0.0f, 0.0f});
    rig.scene.update(kDt);
    checkNear(rig.controller->planarSpeed(), 0.0f, 0.01f, "the solver resumes control");
}

void testImperativeApi() {
    Rig rig;
    rig.controller->launch({0.0f, 6.0f, 3.0f});
    checkNear(rig.body->velocity.y, 6.0f, 0.01f, "launch sets the velocity outright");
    check(rig.controller->isJumping(), "launching upward counts as a jump");

    rig.controller->addImpulse({0.0f, 2.0f, 0.0f});
    checkNear(rig.body->velocity.y, 8.0f, 0.01f, "addImpulse adds to the current velocity");

    rig.controller->faceDirection({1.0f, 0.0f, 0.0f}, true);
    checkNear(std::fabs(rig.controller->facingYawDegrees()), 90.0f, 1.0f,
              "faceDirection snaps when asked");
}

void testLaunchMomentumSurvivesSteering() {
    // Steering during a launch must not delete its speed in one frame, or every
    // move built on `launch` collapses back to walking pace.
    Rig rig;
    rig.controller->moveSpeed = 5.0f;
    rig.controller->groundAcceleration = 25.0f;
    rig.controller->groundDeceleration = 10.0f;
    rig.controller->launch({0.0f, 0.0f, 18.0f});

    rig.controller->setMoveInput({0.0f, -1.0f});  // stick along the launch
    rig.scene.update(kDt);
    check(rig.controller->planarSpeed() > 15.0f,
          "one frame of steering does not erase launch speed");

    for (int i = 0; i < 30; ++i) {
        rig.controller->setMoveInput({0.0f, -1.0f});
        rig.scene.update(kDt);
    }
    const float after = rig.controller->planarSpeed();
    check(after < 15.0f, "the excess bleeds off over time");
    check(after > 4.0f, "and settles toward walking speed rather than stopping");

    // With no braking rate configured the old behaviour stands: instant.
    Rig snappy;
    snappy.controller->moveSpeed = 5.0f;
    snappy.controller->launch({0.0f, 0.0f, 18.0f});
    snappy.controller->setMoveInput({0.0f, -1.0f});
    snappy.scene.update(kDt);
    checkNear(snappy.controller->planarSpeed(), 5.0f, 0.1f,
              "without a braking rate the speed snaps as it always did");
}

void testInputIsOptional() {
    Rig rig;  // readsInput is already false
    rig.scene.update(kDt);
    checkNear(rig.controller->planarSpeed(), 0.0f, 0.01f,
              "a controller that reads no input stays put");
}

void testSprintIsAnIntent() {
    // Reported from play: with readsInput off, sprinting was unreachable — the
    // only source was the key the controller no longer reads.
    Rig rig;
    rig.controller->moveSpeed = 4.0f;
    rig.controller->sprintMultiplier = 2.0f;

    rig.step(5, {0.0f, 1.0f});
    checkNear(rig.controller->planarSpeed(), 4.0f, 0.05f, "walking without a sprint intent");

    for (int i = 0; i < 5; ++i) {
        rig.controller->setMoveInput({0.0f, 1.0f});
        rig.controller->setSprinting(true);
        rig.scene.update(kDt);
    }
    checkNear(rig.controller->planarSpeed(), 8.0f, 0.05f, "setSprinting reaches sprintMultiplier");
    check(rig.controller->isSprinting(), "the sprint intent is readable");

    // It is a per-frame intent like the stick: stop saying it and it lapses.
    rig.step(5, {0.0f, 1.0f});
    checkNear(rig.controller->planarSpeed(), 4.0f, 0.05f, "the sprint intent lapses when not renewed");
}

// CameraFollowBehaviour is NOT covered here, by omission that is deliberate:
// it resolves its target and its occlusion probe through `tree()`, and a
// SceneTree needs a ResourceManager, hence an rhi::Device. Against a bare Scene
// the behaviour returns on its first line, so every assertion would pass on a
// camera that never moved.

}  // namespace

int main() {
    testDefaultsAreUnchanged();
    testGroundAcceleration();
    testGroundDeceleration();
    testJumpHeight();
    testJumpCutoff();
    testCoyoteTime();
    testJumpBuffer();
    testDoubleJump();
    testJumpChain();
    testFallGravityMultiplier();
    testMaxFallSpeed();
    testAirControl();
    testTurnModes();
    testTurnAround();
    testSubclassHooks();
    testSolverCanBeSuspended();
    testImperativeApi();
    testLaunchMomentumSurvivesSteering();
    testInputIsOptional();
    testSprintIsAnIntent();
    std::printf("PASS: character controller (%d checks)\n", gChecks);
    return 0;
}
