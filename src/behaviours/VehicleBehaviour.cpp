#include "behaviours/VehicleBehaviour.hpp"

#include "core/Input.hpp"
#include "core/Log.hpp"
#include "physics/CollisionObjectNode.hpp"
#include "physics/PhysicsWorld.hpp"
#include "physics/RigidBodyNode.hpp"
#include "scene/Node.hpp"
#include "scene/Scene.hpp"
#include "scene/SceneTree.hpp"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <string>

namespace saida {

namespace {

constexpr float kGravity = 9.81f;
// A body whose mass was left to the shape reports none we can read back, so the
// vehicle needs something to scale its forces by. A mid-size car.
constexpr float kFallbackMass = 1200.0f;
// A contact whose normal leans further than this from the vehicle's up is a wall
// or a kerb face, not road: pushing the suspension into it would launch the car.
constexpr float kMinGroundDot = 0.35f;
// Tyres in a driving game are allowed to out-grip physical rubber; this scales
// the Coulomb limit that caps the lateral impulse.
//
// It cannot be raised freely. A car tips when lateral acceleration passes
// g * halfTrack / comHeight — about 1.2 g for a saloon whose mass sits where a
// saloon's does — so a tyre allowed more than that rolls the car over instead of
// sliding it, on flat ground, every time. At 2.0 it did exactly that. Kept below
// the tipping threshold on purpose: a slide is recoverable and fun, a rollover
// is neither. Raise this only together with a lower `centerOfMass`.
constexpr float kFrictionHeadroom = 1.1f;
// The bump stop, as a multiple of the load a wheel carries standing still. Two
// wheels need 2x between them to hold a car up, so this leaves room to corner
// and to land without leaving room to launch.
constexpr float kMaxWheelLoad = 2.5f;

float moveToward(float current, float target, float rate, float dt) {
    // Exponential approach, clamped so a long frame cannot overshoot.
    return current + (target - current) * std::min(1.0f, rate * dt);
}

} // namespace

void VehicleBehaviour::onReady() {
    if (readsInput && !bindingsReady_) {
        // Shares the movement actions, so the same keys and the same stick drive
        // the car and the character. Handbrake takes the jump key.
        Input::bindKey("MoveForward", KeyCode::W);
        Input::bindKey("MoveLeft", KeyCode::A);
        Input::bindKey("MoveBackward", KeyCode::S);
        Input::bindKey("MoveRight", KeyCode::D);
        Input::bindKey("MoveForward", KeyCode::Up);
        Input::bindKey("MoveLeft", KeyCode::Left);
        Input::bindKey("MoveBackward", KeyCode::Down);
        Input::bindKey("MoveRight", KeyCode::Right);
        Input::bindKey("Handbrake", KeyCode::Space);
        bindingsReady_ = true;
    }
    layOutWheels();
}

void VehicleBehaviour::layOutWheels() {
    const float h = wheelAnchorHeight;
    // Left is +X, matching how the car kit names its wheel groups.
    const struct { float x, z; bool front; } slots[4] = {
        { wheelHalfTrack, wheelBaseFront, true},
        {-wheelHalfTrack, wheelBaseFront, true},
        { wheelHalfTrack, -wheelBaseRear, false},
        {-wheelHalfTrack, -wheelBaseRear, false},
    };
    static const char* kSuffix[4] = {"FL", "FR", "RL", "RR"};
    for (int i = 0; i < 4; ++i) {
        wheels_[i].anchor = glm::vec3(slots[i].x, h, slots[i].z);
        wheels_[i].steered = slots[i].front;
        wheels_[i].driven = slots[i].front ? frontWheelDrive : rearWheelDrive;
        wheels_[i].suspensionLength = suspensionRest;
        wheels_[i].visual = node() ? node()->findByPath(wheelNodePrefix + kSuffix[i]) : nullptr;
    }
}

glm::vec2 VehicleBehaviour::readDriveInput() {
    return Input::getVector("MoveLeft", "MoveRight", "MoveBackward", "MoveForward");
}

bool VehicleBehaviour::readHandbrake() {
    return Input::isActionHeld("Handbrake");
}

void VehicleBehaviour::setThrottle(float value) { throttle_ = std::clamp(value, -1.0f, 1.0f); }
void VehicleBehaviour::setBrake(float value) { brake_ = std::clamp(value, 0.0f, 1.0f); }
void VehicleBehaviour::setSteer(float value) { steerInput_ = std::clamp(value, -1.0f, 1.0f); }

float VehicleBehaviour::speed() const { return std::abs(forwardSpeed_); }

void VehicleBehaviour::onUpdate(float frameDt) {
    if (frameDt <= 0.0f || !node()) return;

    // Every force below becomes an impulse by multiplying by dt, so an unbounded
    // frame would apply an unbounded impulse — and the world would not advance
    // far enough to spend it, because PhysicsWorld caps how much it simulates per
    // call. Measured: a 0.63 s frame while the city finished streaming turned a
    // parked car's suspension into 89 kN.s and threw it 28 m into the air. Coast
    // through a hitch instead of being catapulted by it.
    const float dt = std::min(frameDt, PhysicsWorld::kMaxSimulatedStep);

    // The body's own world, not the SceneTree's: they are the same world in a
    // running game, and this one is also there when a Scene is stepped alone.
    auto* collider = dynamic_cast<CollisionObjectNode*>(node());
    if (!collider) return;
    PhysicsWorld* physics = collider->physicsWorld();
    const JPH::BodyID body = collider->bodyId();
    if (!physics || body.IsInvalid()) return;

    float mass = kFallbackMass;
    if (auto* rb = dynamic_cast<RigidBodyNode*>(node()))
        if (rb->mass > 0.0f) mass = rb->mass;

    if (readsInput) {
        const glm::vec2 drive = readDriveInput();
        steerInput_ = std::clamp(drive.x, -1.0f, 1.0f);
        // One axis does both: pushing against the way the car is already moving
        // brakes it, and only brings on reverse once it has stopped.
        const float wanted = std::clamp(drive.y, -1.0f, 1.0f);
        if (wanted * forwardSpeed_ < -0.1f && std::abs(forwardSpeed_) > 0.6f) {
            brake_ = std::abs(wanted);
            throttle_ = 0.0f;
        } else {
            brake_ = 0.0f;
            throttle_ = wanted;
        }
        handbrake_ = readHandbrake();
    }

    // ---- frame of reference ------------------------------------------------
    const glm::mat4 m = node()->worldTransform();
    const glm::vec3 up = glm::normalize(glm::vec3(m[1]));
    const glm::vec3 forward = glm::normalize(glm::vec3(m[2]));

    const glm::vec3 velocity = physics->linearVelocity(body);
    forwardSpeed_ = glm::dot(velocity, forward);

    // ---- steering ----------------------------------------------------------
    const float speedFraction = std::clamp(std::abs(forwardSpeed_) / std::max(1.0f, maxSpeed),
                                           0.0f, 1.0f);
    const float lock = maxSteerAngle * (1.0f - steerSpeedFalloff * speedFraction);
    const float wantedAngle = steerInput_ * lock;
    const float rate = (std::abs(steerInput_) > 0.01f) ? steerSpeed : steerReturnSpeed;
    steerAngle_ = moveToward(steerAngle_, wantedAngle, rate, dt);

    // ---- suspension geometry -----------------------------------------------
    const float wheelMass = mass * 0.25f;
    const float restLoad = wheelMass * kGravity;
    const float springK = suspensionStiffness * restLoad / std::max(0.01f, suspensionRest);
    const float damperC = 2.0f * suspensionDamping * std::sqrt(springK * wheelMass);
    const float rayLength = suspensionRest + suspensionTravel + wheelRadius;

    QueryFilter filter;
    filter.ignore = body;

    struct Contact {
        glm::vec3 point{0.0f};
        glm::vec3 normal{0.0f};
        glm::vec3 forward{0.0f};
        glm::vec3 lateral{0.0f};
        float load = 0.0f;
    } contacts[4];

    int drivenCount = 0;
    for (const Wheel& w : wheels_)
        if (w.driven) ++drivenCount;
    drivenCount = std::max(1, drivenCount);

    wheelsOnGround_ = 0;
    for (int i = 0; i < 4; ++i) {
        Wheel& w = wheels_[i];
        const glm::vec3 anchor = glm::vec3(m * glm::vec4(w.anchor, 1.0f));
        const RaycastHit hit = physics->raycast(anchor, -up, rayLength, filter);

        w.grounded = hit.hit && glm::dot(hit.normal, up) > kMinGroundDot;
        if (!w.grounded) {
            // Hanging: the wheel droops to the end of its travel and keeps
            // spinning, but contributes no force.
            w.suspensionLength = suspensionRest + suspensionTravel;
            w.compression = 0.0f;
            w.load = 0.0f;
            continue;
        }
        ++wheelsOnGround_;

        w.suspensionLength = std::clamp(hit.distance - wheelRadius, 0.0f,
                                        suspensionRest + suspensionTravel);
        w.compression = suspensionRest - w.suspensionLength;

        contacts[i].point = hit.point;
        contacts[i].normal = hit.normal;

        // Damping reads the speed at which the suspension is closing, which is
        // the contact point's velocity along the vehicle's up, negated.
        const glm::vec3 pv = physics->pointVelocity(body, hit.point);
        const float closingRate = -glm::dot(pv, up);

        // A damper removes momentum; it must never inject any. Integrated
        // explicitly, `damperC * closingRate` can exceed the momentum the
        // contact actually carries along the normal, and the excess comes back
        // out as a push: two wheels at the same compression then report loads
        // that differ several-fold, the difference compounds every step, and
        // the car throws itself off the road under power. Cap the damper at the
        // momentum there is to remove, measured against the mass this contact
        // resists with — the same bound the brake below is held to, and the
        // reason `suspensionDamping` can be quoted as a ratio without a stiff
        // spring turning it into an ejector seat.
        const float normalMass = physics->effectiveMassAt(body, hit.point, up);
        const float dampingCeiling = std::abs(closingRate) * normalMass / dt;
        const float damping =
            std::clamp(damperC * closingRate, -dampingCeiling, dampingCeiling);
        float load = springK * w.compression + damping;
        // A spring can only push, and it must not push harder than a suspension
        // can. This is the bump stop, and it is the difference between a car
        // that leans in a corner and one that pole-vaults out of it: as the body
        // rolls, the outer springs compress, and a linear spring answers a deep
        // compression with whatever force the maths gives. Measured at 12x
        // static load, a corner ran the total ground reaction up to 1.56x the
        // car's weight while the inner wheels hung in the air, and the excess
        // went straight into launching it — 1.9 m up, upside down. Real
        // suspension travel ends against a stop long before that.
        load = std::clamp(load, 0.0f, restLoad * kMaxWheelLoad);
        w.load = load;
        contacts[i].load = load;

        // The wheel's heading, steered and then laid flat on the contact plane.
        // NEGATED because the vehicle's +X is its LEFT — that is how the kit
        // names its wheels, and layOutWheels follows it — so a rotation about
        // the vehicle's up by a positive angle swings the nose to the LEFT. A
        // positive steer input means right, so the two have to disagree in sign
        // exactly here. Without it the controls are mirrored, and every test
        // that only checks "the two inputs turn it opposite ways" still passes.
        glm::vec3 heading = forward;
        if (w.steered) {
            const glm::mat4 turn = glm::rotate(glm::mat4(1.0f), glm::radians(-steerAngle_), up);
            heading = glm::vec3(turn * glm::vec4(forward, 0.0f));
        }
        glm::vec3 f = heading - contacts[i].normal * glm::dot(heading, contacts[i].normal);
        if (glm::dot(f, f) < 1e-6f) { w.grounded = false; --wheelsOnGround_; continue; }
        f = glm::normalize(f);
        const glm::vec3 lat = glm::normalize(glm::cross(f, contacts[i].normal));
        contacts[i].forward = f;
        contacts[i].lateral = lat;
    }

    // ---- anti-roll ---------------------------------------------------------
    // A bar across an axle resists that axle's roll, so the load goes TO the
    // wheel that is already squashed and comes off the one hanging: that couple
    // is what pushes the leaning side back up. Signed the other way it is not a
    // weaker bar, it is a divergence — it unloads whichever wheel is compressed
    // and lets it compress further. Measured on a steady turn, the wrong sign
    // took peak lean from 1.4 to 39.6 degrees.
    if (antiRoll > 0.0f) {
        const int axles[2][2] = {{0, 1}, {2, 3}};
        for (const auto& axle : axles) {
            const Wheel& a = wheels_[axle[0]];
            const Wheel& b = wheels_[axle[1]];
            if (!a.grounded || !b.grounded) continue;
            const float transfer = antiRoll * springK * (a.compression - b.compression) * 0.5f;
            contacts[axle[0]].load = std::max(0.0f, contacts[axle[0]].load + transfer);
            contacts[axle[1]].load = std::max(0.0f, contacts[axle[1]].load - transfer);
        }
    }

    // ---- suspension ---------------------------------------------------------
    // The normal forces resolve as one pass, before any tyre is asked what it is
    // slipping at, the way a contact solver resolves normals before friction.
    // Interleaving them is what made a car at rest crawl sideways: a wheel that
    // sampled its contact after only *some* of its neighbours had pushed read
    // their off-centre suspension impulses as slip of its own, and cancelled a
    // rotation nobody had asked for. Four symmetric pushes rotate nothing, so a
    // car at rest now stays exactly where it was put.
    for (int i = 0; i < 4; ++i) {
        if (!wheels_[i].grounded) continue;
        const Contact& c = contacts[i];
        physics->applyImpulse(body, c.normal * (c.load * dt), c.point);
    }

    // A freewheeling wheel owes the physics nothing; its visual still turns.
    for (Wheel& w : wheels_)
        if (!w.grounded) w.spin += (forwardSpeed_ / std::max(0.05f, wheelRadius)) * dt;

    // ---- tyres --------------------------------------------------------------
    // Drive, brake and grip, axle by axle. Sequential BETWEEN axles, so a tyre
    // sees what the tyres before it already took out: solving all four against
    // one state removes `wheels x effectiveMass / mass` times too much — near
    // 105% on this saloon — which reverses the slip and grows it every step
    // until the car throws itself off the road. Simultaneous WITHIN an axle,
    // because its two wheels are mirror images and letting one of a symmetric
    // pair act first is a standing left/right bias that steers a car nobody is
    // steering.
    static const int kAxles[2][2] = {{0, 1}, {2, 3}};
    for (const auto& axle : kAxles) {
        glm::vec3 axleVel[2];
        for (int k = 0; k < 2; ++k)
            axleVel[k] = wheels_[axle[k]].grounded
                             ? physics->pointVelocity(body, contacts[axle[k]].point)
                             : glm::vec3(0.0f);

        for (int k = 0; k < 2; ++k) {
            const int i = axle[k];
            Wheel& w = wheels_[i];
            if (!w.grounded) continue;
            const Contact& c = contacts[i];
            const float vForward = glm::dot(axleVel[k], c.forward);
            const float vLateral = glm::dot(axleVel[k], c.lateral);
            // Fall back to the quarter share only if the body is not dynamic,
            // where no impulse does anything anyway.
            const float massForward = physics->effectiveMassAt(body, c.point, c.forward);
            const float massLateral = physics->effectiveMassAt(body, c.point, c.lateral);
            const float contactMassForward = massForward > 0.0f ? massForward : wheelMass;
            const float contactMassLateral = massLateral > 0.0f ? massLateral : wheelMass;

            // Drive and brake, along the wheel's heading.
            float along = 0.0f;
            if (w.driven && std::abs(throttle_) > 0.001f) {
                // The drive tapers to nothing at top speed instead of being
                // clamped there, so the car settles rather than surging against
                // a wall.
                const float headroom = 1.0f - std::clamp(std::abs(forwardSpeed_) /
                                                         std::max(1.0f, maxSpeed), 0.0f, 1.0f);
                float force = throttle_ * (driveForce / static_cast<float>(drivenCount)) * headroom;
                if (throttle_ < 0.0f) force *= reverseFactor;
                along += force * dt;
            }
            const bool rear = (i >= 2);
            float braking = brake_ * brakeForce * 0.25f;
            if (handbrake_ && rear) braking += handbrakeForce * 0.5f;
            braking += rollingResistance * c.load;
            if (braking > 0.0f) {
                // Never brake past a standstill within one step, or the wheel
                // drags the car backwards. The momentum to remove is measured
                // against the mass this contact actually resists with, not the
                // body's quarter: an impulse applied at the tyre also spins the
                // car, so a quarter of the mass over-corrects, flips the sign
                // every step, and this very clamp then rectifies the
                // oscillation into a steady creep.
                const float stopping = std::abs(vForward) * contactMassForward / dt;
                const float applied = std::min(braking, stopping);
                along -= (vForward > 0.0f ? 1.0f : -1.0f) * applied * dt;
            }
            if (along != 0.0f)
                physics->applyImpulse(body, c.forward * along, c.point);

            // Lateral grip: cancel the sideways slip, within what the load allows.
            float grip = w.steered ? lateralGripFront : lateralGripRear;
            if (handbrake_ && rear) grip = handbrakeGripRear;
            if (tyreLoadSensitivity > 0.0f) {
                // A wheel carrying less than its share grips proportionally
                // less, which is what makes weight transfer change the handling.
                const float share = std::clamp(c.load / std::max(1.0f, restLoad), 0.0f, 2.0f);
                grip *= glm::mix(1.0f, share, std::clamp(tyreLoadSensitivity, 0.0f, 1.0f));
            }
            const float wanted = -vLateral * contactMassLateral * grip;
            const float limit = kFrictionHeadroom * grip * c.load * dt;
            physics->applyImpulse(body, c.lateral * std::clamp(wanted, -limit, limit), c.point);

            w.spin += (vForward / std::max(0.05f, wheelRadius)) * dt;
        }
    }

    // ---- roll stability ----------------------------------------------------
    // Springs alone cannot hold a car up once it leans far enough: the outer
    // pair is at full compression and the inner pair carries nothing, so the
    // restoring couple stops growing exactly when it is most needed and the car
    // goes over. This is the stabiliser bar the model does not otherwise have.
    // It only acts while a wheel is down, so a jump or a ramp is left alone, and
    // it pulls toward the surface the car is standing on rather than toward
    // world up — on a banked road those are not the same thing.
    if ((rollStability > 0.0f || rollDamping > 0.0f) && wheelsOnGround_ > 0) {
        glm::vec3 groundUp(0.0f);
        for (int i = 0; i < 4; ++i)
            if (wheels_[i].grounded) groundUp += contacts[i].normal;
        if (glm::dot(groundUp, groundUp) > 1e-6f) {
            groundUp = glm::normalize(groundUp);
            // up x groundUp is the axis that rotates the car back onto the
            // surface, and its length is the sine of the lean.
            const glm::vec3 axis = glm::cross(up, groundUp);
            const float lean = glm::length(axis);
            const float tonnes = mass / 1000.0f;
            glm::vec3 torque(0.0f);
            if (lean > 1e-4f) torque += (axis / lean) * (rollStability * tonnes * lean);
            // Damped about the car's own length, which is the axis it rolls on.
            const glm::vec3 spin = physics->angularVelocity(body);
            torque -= forward * (glm::dot(spin, forward) * rollDamping * tonnes);
            physics->applyAngularImpulse(body, torque * dt);
        }
    }

    // ---- righting an overturned car ----------------------------------------
    // A raycast vehicle on its roof has nothing under its wheels to push
    // against, so it stays there for ever and the only way out is to reload.
    // Holding a steering direction rolls it back about its own length, the way
    // every open-world driving game does it. Gated on being genuinely overturned
    // AND off its wheels, so this can never help a car that is merely leaning.
    const float uprightness = glm::dot(up, glm::vec3(0.0f, 1.0f, 0.0f));
    if (selfRightTorque > 0.0f && wheelsOnGround_ == 0 &&
        uprightness < selfRightThreshold && std::abs(steerInput_) > 0.01f) {
        const glm::vec3 spin = physics->angularVelocity(body);
        // Capped so it rocks back rather than winding up into a spin the player
        // then has to wait out.
        if (glm::dot(spin, forward) * -steerInput_ < selfRightMaxSpin) {
            const float torque = selfRightTorque * (mass / 1000.0f) * steerInput_;
            // Negative for the same reason the steering is: +X is the car's
            // left, so a positive roll about its forward drops the left side.
            physics->applyAngularImpulse(body, forward * (-torque * dt));
        }
    }

    // ---- body forces -------------------------------------------------------
    const float speedNow = glm::length(velocity);
    // Drag is airDrag * v^2 against the direction of travel, so the impulse is
    // -velocity * speed * airDrag * dt.
    if (airDrag > 0.0f && speedNow > 0.01f)
        physics->applyImpulse(body, -velocity * (speedNow * airDrag * dt));
    if (downforce > 0.0f && wheelsOnGround_ > 0)
        physics->applyImpulse(body, -up * (speedNow * speedNow * downforce * dt));

    updateVisuals();
}

void VehicleBehaviour::updateVisuals() {
    static const bool kMirrored[4] = {false, true, false, true};
    for (int i = 0; i < 4; ++i) {
        Wheel& w = wheels_[i];
        if (!w.visual) continue;
        // The wheel hangs below its anchor by however far the suspension is
        // extended; the mesh is centred on its own axle, so this is its centre.
        w.visual->transform().position =
            glm::vec3(w.anchor.x, w.anchor.y - w.suspensionLength, w.anchor.z);

        // One mesh serves both sides, so the right-hand wheels are turned about
        // to mirror it — which also reverses the axis they roll about. The
        // steering angle is negated for the same reason as in onUpdate: the
        // vehicle's +X is its left, so the drawn wheels must point the way the
        // car actually goes rather than mirroring it.
        const float yaw = (w.steered ? -steerAngle_ : 0.0f) + (kMirrored[i] ? 180.0f : 0.0f);
        const float spin = kMirrored[i] ? -w.spin : w.spin;
        w.visual->transform().rotation =
            glm::angleAxis(glm::radians(yaw), glm::vec3(0.0f, 1.0f, 0.0f)) *
            glm::angleAxis(spin, glm::vec3(1.0f, 0.0f, 0.0f));
    }
}

void VehicleBehaviour::describe(reflect::TypeBuilder<VehicleBehaviour>& t) {
    t.doc("Raycast vehicle. Attach to a RigidBody whose box sits above the ground: each "
          "wheel is a ray, and suspension, drive and grip are applied at the contact so the "
          "body gets its weight transfer for free. Forward is local +Z. Turn off readsInput "
          "and drive it through setThrottle/setBrake/setSteer to run it from an AI.");

    t.property("wheelHalfTrack", &VehicleBehaviour::wheelHalfTrack).range(0.2, 3.0);
    t.property("wheelBaseFront", &VehicleBehaviour::wheelBaseFront).range(0.2, 6.0);
    t.property("wheelBaseRear", &VehicleBehaviour::wheelBaseRear).range(0.2, 6.0);
    t.property("wheelRadius", &VehicleBehaviour::wheelRadius).range(0.05, 1.5);
    t.property("wheelAnchorHeight", &VehicleBehaviour::wheelAnchorHeight).range(0.0, 3.0)
        .tooltip("height above the body origin the suspension hangs from");

    t.property("suspensionRest", &VehicleBehaviour::suspensionRest).range(0.05, 1.5);
    t.property("suspensionTravel", &VehicleBehaviour::suspensionTravel).range(0.0, 1.0);
    t.property("suspensionStiffness", &VehicleBehaviour::suspensionStiffness).range(1.0, 60.0)
        .tooltip("higher is stiffer; the car sags suspensionRest/stiffness at rest");
    t.property("suspensionDamping", &VehicleBehaviour::suspensionDamping).range(0.0, 6.0)
        .tooltip("damping ratio: 1 is critical, so it stays tuned when the mass changes");

    t.property("driveForce", &VehicleBehaviour::driveForce).range(0.0, 60000.0);
    t.property("brakeForce", &VehicleBehaviour::brakeForce).range(0.0, 60000.0);
    t.property("handbrakeForce", &VehicleBehaviour::handbrakeForce).range(0.0, 60000.0);
    t.property("maxSpeed", &VehicleBehaviour::maxSpeed).range(1.0, 120.0);
    t.property("reverseFactor", &VehicleBehaviour::reverseFactor).range(0.0, 1.0);
    t.property("rollingResistance", &VehicleBehaviour::rollingResistance).range(0.0, 0.2);
    t.property("airDrag", &VehicleBehaviour::airDrag).range(0.0, 5.0);

    t.property("maxSteerAngle", &VehicleBehaviour::maxSteerAngle).range(5.0, 60.0);
    t.property("steerSpeed", &VehicleBehaviour::steerSpeed).range(0.5, 30.0);
    t.property("steerSpeedFalloff", &VehicleBehaviour::steerSpeedFalloff).range(0.0, 0.95);
    t.property("steerReturnSpeed", &VehicleBehaviour::steerReturnSpeed).range(0.5, 30.0);

    t.property("lateralGripFront", &VehicleBehaviour::lateralGripFront).range(0.0, 1.0);
    t.property("lateralGripRear", &VehicleBehaviour::lateralGripRear).range(0.0, 1.0);
    t.property("handbrakeGripRear", &VehicleBehaviour::handbrakeGripRear).range(0.0, 1.0);
    t.property("tyreLoadSensitivity", &VehicleBehaviour::tyreLoadSensitivity).range(0.0, 1.0);

    t.property("downforce", &VehicleBehaviour::downforce).range(0.0, 20.0);
    t.property("antiRoll", &VehicleBehaviour::antiRoll).range(0.0, 1.0);
    t.property("rollStability", &VehicleBehaviour::rollStability).range(0.0, 30000.0)
        .tooltip("N.m per tonne pulling the car back onto the surface it stands on; 0 disables");
    t.property("rollDamping", &VehicleBehaviour::rollDamping).range(0.0, 10000.0)
        .tooltip("N.m per tonne per (rad/s), bleeding off roll rate while grounded");

    t.property("selfRightTorque", &VehicleBehaviour::selfRightTorque).range(0.0, 40000.0)
        .tooltip("N.m per tonne when steering is held and the car is on its roof; 0 disables");
    t.property("selfRightThreshold", &VehicleBehaviour::selfRightThreshold).range(-1.0, 1.0)
        .tooltip("cosine of the tilt that counts as overturned; 0.35 is about 70 degrees");
    t.property("selfRightMaxSpin", &VehicleBehaviour::selfRightMaxSpin).range(0.1, 10.0);

    t.property("wheelNodePrefix", &VehicleBehaviour::wheelNodePrefix)
        .tooltip("child nodes <prefix>FL/FR/RL/RR are placed, steered and spun");
    t.property("frontWheelDrive", &VehicleBehaviour::frontWheelDrive);
    t.property("rearWheelDrive", &VehicleBehaviour::rearWheelDrive);
    t.property("readsInput", &VehicleBehaviour::readsInput);
}

} // namespace saida
