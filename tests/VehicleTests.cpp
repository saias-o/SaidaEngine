// Headless proof of VehicleBehaviour, the raycast vehicle:
//   - it holds itself up on its suspension at the height the geometry implies,
//     with all four wheels reporting ground;
//   - throttle accelerates it along its own forward, brake brings it back to a
//     standstill, and neither drives it through the floor;
//   - steering turns the body, and the direction of the turn follows the sign of
//     the steering input;
//   - it is driven entirely through the imperative API with readsInput off,
//     which is the path the traffic system uses;
//   - a wheel over a hole is reported airborne instead of inventing grip;
//   - nothing produces a NaN.
#include "behaviours/VehicleBehaviour.hpp"
#include "physics/CollisionShapeNode.hpp"
#include "physics/PhysicsWorld.hpp"
#include "physics/RigidBodyNode.hpp"
#include "physics/StaticBodyNode.hpp"
#include "scene/Scene.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

#include <glm/gtc/epsilon.hpp>

using namespace saida;

namespace {

int gChecks = 0;

void require(bool condition, const char* what) {
    ++gChecks;
    if (!condition) {
        std::printf("[vehicle] FAIL: %s\n", what);
        std::abort();
    }
}

void requireNear(float actual, float expected, float tolerance, const char* what) {
    ++gChecks;
    if (!(std::fabs(actual - expected) <= tolerance)) {
        std::printf("[vehicle] FAIL: %s (got %f, expected %f +/- %f)\n", what, actual,
                    expected, tolerance);
        std::abort();
    }
}

bool finite(const glm::vec3& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

// A sedan from the car kit at the scale the city uses: wheels of radius 0.375 m
// on a 1.06 m track and a 1.65 m wheelbase, chassis box clear of the ground.
struct Rig {
    Scene scene;
    RigidBodyNode* body = nullptr;
    VehicleBehaviour* vehicle = nullptr;

    // A floor slab spanning [x0,x1] x [z0,z1], its top face at y = 0.
    void addSlab(const char* name, float x0, float x1, float z0, float z1) {
        auto* slab = scene.createChild<StaticBodyNode>();
        slab->setName(name);
        slab->transform().position = {(x0 + x1) * 0.5f, -1.0f, (z0 + z1) * 0.5f};
        auto shape = std::make_unique<CollisionShapeNode>();
        shape->shapeType = CollisionShapeType::Box;
        shape->halfExtents = {(x1 - x0) * 0.5f, 1.0f, (z1 - z0) * 0.5f};
        slab->addChild(std::move(shape));
    }

    explicit Rig(bool withHole = false) {
        if (!withHole) {
            // Big enough that a car cannot drive off it. Forty seconds at full
            // throttle covers well over a kilometre, and a car that reaches the
            // edge falls, tumbles, and reports a speed with its fall in it —
            // which reads exactly like a top speed that was never capped.
            addSlab("Floor", -2000.0f, 2000.0f, -2000.0f, 2000.0f);
        } else {
            // A hole is an ABSENCE of floor. A body added under a solid floor
            // changes nothing a downward ray can see — it stops on the floor
            // above it — so the gap is left by the slabs themselves. 0.70 m
            // square, centred under the front-left wheel at (+0.53, +0.825),
            // which leaves the other three carrying the car level.
            addSlab("FloorLeft", -2000.0f, 0.18f, -2000.0f, 2000.0f);
            addSlab("FloorRight", 0.88f, 2000.0f, -2000.0f, 2000.0f);
            addSlab("FloorNear", 0.18f, 0.88f, -2000.0f, 0.475f);
            addSlab("FloorFar", 0.18f, 0.88f, 1.175f, 2000.0f);
        }

        body = scene.createChild<RigidBodyNode>();
        body->setName("Car");
        // Started at the height its springs settle to. Dropping it in would make
        // every measurement below start with a landing slide instead.
        body->transform().position = {0.0f, 0.02f, 0.0f};
        body->mass = 1200.0f;
        body->linearDamping = 0.0f;
        body->angularDamping = 0.05f;
        auto chassis = std::make_unique<CollisionShapeNode>();
        chassis->shapeType = CollisionShapeType::Box;
        chassis->halfExtents = {0.94f, 0.72f, 1.60f};
        // Lifted so the box never reaches the road: the wheels carry the car.
        chassis->offset = {0.0f, 0.91f, 0.0f};
        body->addChild(std::move(chassis));
        // A car's mass is in its floorpan, engine and occupants, not spread
        // through the box that has to cover its roof. Left at the box centre
        // (0.91 m) it tips at 0.59 g, under what its own tyres pull, so it rolls
        // over in any hard corner. This puts it at 0.50 m, where a saloon's is.
        body->centerOfMass = {0.0f, -0.556f, 0.0f};

        vehicle = body->addBehaviour<VehicleBehaviour>();
        vehicle->readsInput = false;
        vehicle->wheelHalfTrack = 0.53f;
        vehicle->wheelBaseFront = 0.825f;
        vehicle->wheelBaseRear = 0.825f;
        vehicle->wheelRadius = 0.375f;
        vehicle->suspensionRest = 0.35f;
        // Anchor = radius + the length the spring settles to, so the body rests
        // at the height the model was authored at.
        vehicle->wheelAnchorHeight = 0.70f;
    }

    void step(float seconds) {
        const float dt = 1.0f / 60.0f;
        const int frames = static_cast<int>(seconds * 60.0f);
        for (int i = 0; i < frames; ++i) scene.update(dt);
    }

    glm::vec3 position() const { return body->transform().position; }
    // Roll in degrees: how far the body's own right vector has tilted out of
    // the horizontal plane.
    float rollDegrees() const {
        const glm::mat4 m = body->worldTransform();
        return glm::degrees(std::asin(std::clamp(glm::vec3(m[0]).y, -1.0f, 1.0f)));
    }
    // How upright the car is: +1 on its wheels, 0 on its side, -1 on its roof.
    float uprightness() const {
        const glm::mat4 m = body->worldTransform();
        return glm::normalize(glm::vec3(m[1])).y;
    }
    // Heading in the XZ plane, in degrees, from the body's own forward (+Z).
    float headingDegrees() const {
        const glm::mat4 m = body->worldTransform();
        const glm::vec3 f = glm::vec3(m[2]);
        return glm::degrees(std::atan2(f.x, f.z));
    }
};

// ---- resting on its springs ------------------------------------------------

void testSettlesOnSuspension() {
    Rig rig;
    rig.step(3.0f);

    const glm::vec3 p = rig.position();
    std::printf("[vehicle]    rest: pos=(%.3f, %.3f, %.3f) wheels=%d speed=%.3f\n",
                p.x, p.y, p.z, rig.vehicle->wheelsOnGround(), rig.vehicle->forwardSpeed());
    require(finite(p), "the resting position is finite");
    require(rig.vehicle->wheelsOnGround() == 4, "all four wheels find the road");
    // Equilibrium compresses the spring by rest/stiffness = 0.35/14 = 0.025 m,
    // so the body origin settles just under the height it was authored at.
    requireNear(p.y, 0.0f, 0.08f, "the body rests at its authored height");
    requireNear(p.x, 0.0f, 0.05f, "a car at rest does not drift sideways");
    requireNear(p.z, 0.0f, 0.05f, "a car at rest does not creep forward");
    requireNear(rig.vehicle->forwardSpeed(), 0.0f, 0.1f, "a car at rest reports no speed");
}

// ---- throttle and brake ----------------------------------------------------

void testThrottleAccelerates() {
    Rig rig;
    rig.step(1.5f);
    const float z0 = rig.position().z;

    rig.vehicle->setThrottle(1.0f);
    rig.step(4.0f);

    const glm::vec3 p = rig.position();
    require(finite(p), "the driven position is finite");
    require(rig.vehicle->forwardSpeed() > 5.0f, "full throttle builds real speed");
    require(p.z - z0 > 5.0f, "the car travels along its own forward, +Z");
    requireNear(p.x, 0.0f, 1.0f, "driving straight does not wander sideways");
    require(p.y > -0.3f, "the car does not sink through the road under power");
}

void testBrakeStops() {
    Rig rig;
    rig.step(1.0f);
    rig.vehicle->setThrottle(1.0f);
    rig.step(4.0f);
    require(rig.vehicle->speed() > 5.0f, "the car is moving before it brakes");

    rig.vehicle->setThrottle(0.0f);
    rig.vehicle->setBrake(1.0f);
    rig.step(5.0f);

    require(rig.vehicle->speed() < 0.5f, "the brake brings the car to a standstill");
    // The brake must not drag it backwards once it has stopped.
    const float z = rig.position().z;
    rig.step(2.0f);
    requireNear(rig.position().z, z, 0.2f, "a braked car stays stopped");
}

void testTopSpeedIsBounded() {
    Rig rig;
    rig.step(1.0f);
    rig.vehicle->setThrottle(1.0f);
    rig.step(40.0f);
    std::printf("[vehicle]    after 40 s: %.2f m/s (max %.1f) y=%.3f up=%.3f wheels=%d\n",
                rig.vehicle->speed(), rig.vehicle->maxSpeed, rig.position().y,
                rig.uprightness(), rig.vehicle->wheelsOnGround());
    require(rig.vehicle->speed() <= rig.vehicle->maxSpeed + 1.0f,
            "drive tapers off rather than accelerating without limit");
    require(finite(rig.position()), "a long run stays finite");
}

// ---- steering --------------------------------------------------------------

// Which way is which, not merely that the two differ. Asserting only that
// opposite inputs give opposite turns is what let the controls ship inverted:
// the car turned, it turned both ways, and every check passed.
//
// The convention: forward is local +Z and +X is the car's LEFT, which is how the
// kit names its wheels (wheel-front-left sits at +x). So the car's right is -X,
// and steering right must swing the nose toward -X — a falling headingDegrees,
// since that is atan2(forward.x, forward.z).
// Accumulated frame by frame rather than as a difference of two headings:
// headingDegrees is an atan2 and wraps at 180, so a car that turns further than
// a half-circle — which one on grippy tyres does in three seconds — reports a
// turn of the wrong sign. Measuring the total this way survives any number of
// full turns.
float turnWhileSteering(Rig& rig, float steer, float seconds) {
    rig.step(1.0f);
    rig.vehicle->setThrottle(1.0f);
    rig.vehicle->setSteer(steer);
    float previous = rig.headingDegrees();
    float total = 0.0f;
    const int frames = static_cast<int>(seconds * 60.0f);
    for (int i = 0; i < frames; ++i) {
        rig.step(1.0f / 60.0f);
        float delta = rig.headingDegrees() - previous;
        while (delta > 180.0f) delta -= 360.0f;
        while (delta < -180.0f) delta += 360.0f;
        total += delta;
        previous = rig.headingDegrees();
    }
    return total;
}

void testSteeringTurnsTheCar() {
    Rig leftRig;
    const float leftTurn = turnWhileSteering(leftRig, -1.0f, 3.0f);
    Rig rightRig;
    const float rightTurn = turnWhileSteering(rightRig, 1.0f, 3.0f);

    std::printf("[vehicle]    steer -1 turns %+.1f deg, steer +1 turns %+.1f deg\n",
                leftTurn, rightTurn);
    require(std::fabs(leftTurn) > 5.0f, "steering actually turns the car");
    require(leftTurn * rightTurn < 0.0f,
            "opposite steering turns the car in opposite directions");
    require(rightTurn < 0.0f, "steering right takes the nose to the car's right");
    require(leftTurn > 0.0f, "steering left takes the nose to the car's left");
}

// A car may slide, and a car may be tipped by a kerb or a ramp. What it must not
// do is roll over because a tyre gripped: with the centre of mass at the middle
// of the body box the tipping threshold was 0.59 g while the tyres were allowed
// 2 g, so a hard corner on flat ground put it on its roof every time.
void testHardCorneringDoesNotRollTheCar() {
    Rig rig;
    rig.step(1.0f);
    rig.vehicle->setThrottle(1.0f);
    rig.step(3.0f);              // up to speed first
    rig.vehicle->setSteer(1.0f); // then everything the wheel has

    float peak = 0.0f;
    for (int i = 0; i < 300; ++i) {
        rig.step(1.0f / 60.0f);
        peak = std::max(peak, std::fabs(rig.rollDegrees()));
    }
    std::printf("[vehicle]    peak roll at full lock and full throttle: %.1f deg\n", peak);
    require(std::isfinite(peak), "a hard corner stays finite");
    require(peak < 35.0f, "a hard corner slides the car rather than rolling it");
    require(rig.vehicle->wheelsOnGround() >= 2,
            "it keeps wheels on the road through the corner");
}

// Being upside down must not be a dead end. GTA lets a player rock a car back
// onto its wheels by holding a direction; without it the only way out of a roll
// is to reload.
void testAnOverturnedCarCanBeRighted() {
    Rig rig;
    // Dropped in on its roof, which is where a player ends up.
    rig.body->transform().position = {0.0f, 1.2f, 0.0f};
    rig.body->transform().rotation = glm::angleAxis(glm::radians(180.0f),
                                                   glm::vec3(0.0f, 0.0f, 1.0f));
    rig.step(2.0f);
    const float upsideDown = rig.uprightness();
    std::printf("[vehicle]    dropped on its roof: uprightness %.2f, wheels %d\n",
                upsideDown, rig.vehicle->wheelsOnGround());
    require(upsideDown < 0.0f, "the car really is upside down to begin with");

    // Hold a direction, as a player would.
    rig.vehicle->setSteer(1.0f);
    for (int i = 0; i < 600 && rig.uprightness() < 0.7f; ++i)
        rig.step(1.0f / 60.0f);

    std::printf("[vehicle]    after holding a direction: uprightness %.2f, wheels %d\n",
                rig.uprightness(), rig.vehicle->wheelsOnGround());
    require(rig.uprightness() > 0.7f, "holding a direction rights an overturned car");
    require(finite(rig.position()), "righting itself stays finite");

    // And once back on its wheels it must settle rather than keep spinning.
    rig.vehicle->setSteer(0.0f);
    rig.step(2.0f);
    require(rig.uprightness() > 0.7f, "it stays upright once it is");
}

void testSteeringSelfCentres() {
    Rig rig;
    rig.step(1.0f);
    rig.vehicle->setThrottle(1.0f);
    rig.vehicle->setSteer(1.0f);
    rig.step(2.0f);
    rig.vehicle->setSteer(0.0f);
    rig.step(3.0f);
    const float a = rig.headingDegrees();
    rig.step(1.5f);
    // With the wheels back at centre the heading stops changing.
    requireNear(rig.headingDegrees(), a, 6.0f, "released steering settles straight");
}

// ---- ground reporting ------------------------------------------------------

void testWheelOverAHoleIsAirborne() {
    Rig rig(true);
    rig.step(3.0f);
    require(rig.vehicle->wheelsOnGround() < 4,
            "a wheel with nothing under it is not reported as grounded");
    require(rig.vehicle->wheelsOnGround() >= 2,
            "the wheels that do have road under them still carry the car");
    require(finite(rig.position()), "an uneven contact set stays finite");
}

// A damper removes momentum; it must never inject any. Explicitly integrated,
// c * closingRate grows with the damping ratio while the momentum available at
// the contact does not, so past a point the excess comes back out as a push.
// The vehicle bounds it, which is what lets suspensionDamping stay a ratio the
// author picks rather than a fuse they have to know about.
void testOverDampedSuspensionStaysPut() {
    Rig rig;
    rig.vehicle->suspensionDamping = 12.0f;  // ~12x critical, far past any tuning
    rig.step(3.0f);

    const glm::vec3 p = rig.position();
    std::printf("[vehicle]    over-damped: pos=(%.3f, %.3f, %.3f) wheels=%d\n",
                p.x, p.y, p.z, rig.vehicle->wheelsOnGround());
    require(finite(p), "an over-damped suspension stays finite");
    require(rig.vehicle->wheelsOnGround() == 4, "it keeps all four wheels on the road");
    requireNear(p.y, 0.0f, 0.08f, "it is not launched off its own springs");
    requireNear(p.x, 0.0f, 0.05f, "and it does not walk sideways doing it");
}

// The contract the property claims: a bar across an axle resists that axle's
// roll. It is worth pinning because a bar wired the other way round is not
// inert, it is a divergence — it takes load off whichever wheel is already
// squashed, and the car leans further for it.
void testAntiRollReducesBodyRoll() {
    // A measured turn, not a stunt. This rig is a tall narrow one — centre of
    // mass 0.91 m up over a 1.06 m track — and tyres allowed to pull 2 g, so at
    // full lock and full throttle it puts itself on its roof, which is correct
    // and tells us nothing about a bar. Peak lean through a steady turn does.
    auto peakRollThroughATurn = [](float antiRoll) {
        Rig rig;
        rig.vehicle->antiRoll = antiRoll;
        rig.step(1.0f);
        rig.vehicle->setThrottle(0.45f);
        rig.vehicle->setSteer(0.30f);
        float peak = 0.0f;
        for (int i = 0; i < 150; ++i) {
            rig.step(1.0f / 60.0f);
            peak = std::max(peak, std::fabs(rig.rollDegrees()));
        }
        return peak;
    };

    const float loose = peakRollThroughATurn(0.0f);
    const float barred = peakRollThroughATurn(0.9f);
    std::printf("[vehicle]    peak roll in a turn: no bar %.3f deg, bar %.3f deg\n",
                loose, barred);
    require(std::isfinite(loose) && std::isfinite(barred), "both runs stay finite");
    require(loose < 45.0f, "the measured turn keeps the car on its wheels");
    require(barred < loose, "an anti-roll bar leans the car less, not more");
}

// The visuals are the only half of this behaviour a player ever sees, and until
// a rig carries wheel nodes nothing exercises them at all: findByPath returns
// null for each, updateVisuals writes nowhere, and a wheel drawn in the wrong
// place, mirrored the wrong way or spinning backwards passes every other test
// here. This rig gives it four to move.
void testWheelNodesFollowTheSuspension() {
    Rig rig;
    Node* wheels[4] = {nullptr, nullptr, nullptr, nullptr};
    static const char* kSuffix[4] = {"FL", "FR", "RL", "RR"};
    for (int i = 0; i < 4; ++i) {
        auto child = std::make_unique<Node>();
        child->setName(std::string("Wheel") + kSuffix[i]);
        wheels[i] = child.get();
        rig.body->addChild(std::move(child));
    }
    // onReady has already run without them, so re-resolve by re-readying.
    rig.vehicle->onReady();
    rig.step(2.0f);

    const float anchorY = rig.vehicle->wheelAnchorHeight;
    const float halfTrack = rig.vehicle->wheelHalfTrack;
    const float base = rig.vehicle->wheelBaseFront;
    for (int i = 0; i < 4; ++i) {
        const glm::vec3 p = wheels[i]->transform().position;
        require(std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z),
                "a wheel node is placed at a finite point");
        // Left is +X and the front axle is +Z, matching how the kit names them.
        requireNear(p.x, (i == 0 || i == 2) ? halfTrack : -halfTrack, 1e-3f,
                    "a wheel sits on its own side of the car");
        requireNear(p.z, (i < 2) ? base : -base, 1e-3f,
                    "a wheel sits on its own axle");
        // Hanging from the anchor by the length the spring settled to, which at
        // equilibrium is rest minus rest/stiffness.
        const float settled = rig.vehicle->suspensionRest *
                              (1.0f - 1.0f / rig.vehicle->suspensionStiffness);
        requireNear(p.y, anchorY - settled, 0.02f,
                    "a wheel hangs where its suspension puts it");
    }
    std::printf("[vehicle]    wheel FL local: (%.3f, %.3f, %.3f)\n",
                wheels[0]->transform().position.x, wheels[0]->transform().position.y,
                wheels[0]->transform().position.z);

    // Driving must spin them, and the right-hand pair is turned about to mirror
    // one shared mesh, so its spin runs the other way.
    const glm::quat before = wheels[0]->transform().rotation;
    rig.vehicle->setThrottle(1.0f);
    rig.step(1.5f);
    require(glm::any(glm::epsilonNotEqual(
                glm::vec4(wheels[0]->transform().rotation.x, wheels[0]->transform().rotation.y,
                          wheels[0]->transform().rotation.z, wheels[0]->transform().rotation.w),
                glm::vec4(before.x, before.y, before.z, before.w), 1e-4f)),
            "a driven wheel spins");
}

void testAirborneVehicleFalls() {
    Rig rig;
    // Well above the road, so no ray reaches it.
    rig.body->transform().position = {0.0f, 12.0f, 0.0f};
    rig.step(0.5f);
    require(rig.vehicle->wheelsOnGround() == 0, "a car in the air has no wheels down");
    require(!rig.vehicle->isGrounded(), "isGrounded agrees with the wheel count");
    rig.step(3.0f);
    require(rig.position().y < 6.0f, "an airborne car falls");
    rig.step(4.0f);
    require(rig.vehicle->wheelsOnGround() == 4, "it lands back on all four wheels");
    require(finite(rig.position()), "the landing stays finite");
}

} // namespace

int main() {
    // Unbuffered: a crash mid-test would otherwise swallow the line that says
    // which check was running.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    struct Case { const char* name; void (*run)(); };
    const Case cases[] = {
        {"settles on suspension", testSettlesOnSuspension},
        {"throttle accelerates", testThrottleAccelerates},
        {"brake stops", testBrakeStops},
        {"top speed is bounded", testTopSpeedIsBounded},
        {"steering turns the car", testSteeringTurnsTheCar},
        {"hard cornering does not roll the car", testHardCorneringDoesNotRollTheCar},
        {"an overturned car can be righted", testAnOverturnedCarCanBeRighted},
        {"steering self-centres", testSteeringSelfCentres},
        {"wheel over a hole is airborne", testWheelOverAHoleIsAirborne},
        {"over-damped suspension stays put", testOverDampedSuspensionStaysPut},
        {"anti-roll reduces body roll", testAntiRollReducesBodyRoll},
        {"wheel nodes follow the suspension", testWheelNodesFollowTheSuspension},
        {"airborne vehicle falls", testAirborneVehicleFalls},
    };
    for (const Case& c : cases) {
        std::printf("[vehicle] .. %s\n", c.name);
        c.run();
    }
    std::printf("[vehicle] PASS (%d checks)\n", gChecks);
    return 0;
}
