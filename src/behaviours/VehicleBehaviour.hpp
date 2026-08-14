#pragma once

#include "core/Reflection.hpp"
#include "scene/Behaviour.hpp"

#include <glm/glm.hpp>

#include <string>

namespace saida {

class PhysicsWorld;

// A raycast vehicle (cf. Jolt's VehicleConstraint / Unity's WheelCollider).
//
// Attach to a RigidBody whose collision box sits ABOVE the ground: the chassis
// never touches it. Each of the four wheels is a ray cast down from its anchor,
// and what the ray finds drives three impulses applied AT THE CONTACT POINT —
// suspension along the contact normal, drive or brake along the wheel's heading,
// and a lateral impulse that cancels sideways slip. Applying them at the contact
// rather than at the centre of mass is what gives the body its weight transfer:
// the nose dives under braking and the car rolls in a turn for free.
//
// The vehicle's forward is local +Z, matching how the car models are authored.
//
// Like CharacterBehaviour, it has two entry points. Leave `readsInput` on and it
// drives itself from the shared movement actions; turn it off and drive it
// through setThrottle/setBrake/setSteer, which is how the traffic system runs a
// street full of these without a second vehicle implementation.
class VehicleBehaviour : public Behaviour {
public:
    ~VehicleBehaviour() override = default;

    void onReady() override;
    void onUpdate(float frameDt) override;

    // ---- layout (metres, in the body's local frame) -------------------------
    // Measured off the art by tools/split_car_wheels.py rather than typed in.
    float wheelHalfTrack = 0.53f;   // lateral distance from centreline to a wheel
    float wheelBaseFront = 0.83f;   // front axle, ahead of the body origin
    float wheelBaseRear = 0.83f;    // rear axle, behind it
    float wheelRadius = 0.375f;
    float wheelAnchorHeight = 0.8f; // where the suspension hangs from

    // ---- suspension --------------------------------------------------------
    float suspensionRest = 0.35f;   // anchor-to-wheel-centre at rest
    float suspensionTravel = 0.22f; // extra droop the ray still reaches through
    // Stiffness is quoted per wheel as a multiple of the weight it carries, so a
    // vehicle stays tuned when its mass changes: 1 holds the car at rest exactly
    // at `suspensionRest`, and higher compresses less.
    float suspensionStiffness = 14.0f;
    float suspensionDamping = 2.6f; // N.s/m per (m/s), also scaled by weight

    // ---- drive -------------------------------------------------------------
    float driveForce = 7000.0f;     // N at full throttle, shared by driven wheels
    float brakeForce = 9000.0f;
    float handbrakeForce = 6000.0f; // rear only, and it drops rear grip
    float maxSpeed = 32.0f;         // m/s; drive tapers to nothing here
    float reverseFactor = 0.4f;     // reverse is weaker than forward
    float rollingResistance = 0.012f;
    float airDrag = 0.42f;          // N per (m/s)^2

    // ---- steering ----------------------------------------------------------
    float maxSteerAngle = 34.0f;    // degrees at a standstill
    float steerSpeed = 5.0f;        // how fast the wheels reach the wanted angle
    // Full lock at speed would spin the car, so the limit falls off with speed.
    float steerSpeedFalloff = 0.6f; // fraction of the lock lost by maxSpeed
    float steerReturnSpeed = 7.0f;  // self-centring when the input is released

    // ---- tyres -------------------------------------------------------------
    // Grip is a fraction of the sideways slip cancelled per second; 1 is a rail.
    float lateralGripFront = 0.92f;
    float lateralGripRear = 0.88f;
    float handbrakeGripRear = 0.30f;  // what the rear keeps while sliding
    float tyreLoadSensitivity = 1.0f; // >0 ties grip to how loaded the wheel is

    // ---- stability ---------------------------------------------------------
    float downforce = 0.0f;         // N per (m/s)^2 pressed into the road
    float antiRoll = 0.4f;          // 0..1, evens out load across each axle
    // Keeps the car on its wheels. A raycast vehicle has nothing damping roll
    // except its own springs, and at the limit those are already at full
    // compression on one side and carrying nothing on the other — so past a
    // point there is no restoring force left at all and the car simply goes
    // over. This is the stabiliser the model otherwise lacks: a torque toward
    // the surface it is standing on, applied only while a wheel is down, so it
    // never fights a jump or a ramp. Quoted per tonne. 0 disables it.
    float rollStability = 13000.0f;   // N.m per 1000 kg at 90 degrees of lean
    float rollDamping = 1400.0f;     // N.m per 1000 kg per (rad/s) of roll rate

    // ---- righting an overturned car ----------------------------------------
    // On its roof with no wheels down, a raycast vehicle has nothing to push
    // against and stays there for ever: without this the only way out of a roll
    // is to reload. Holding a steering direction rocks it back, which is the
    // convention every open-world driving game uses.
    // Torque is quoted per tonne so a lorry rights as readily as a hatchback.
    float selfRightTorque = 22000.0f;  // N.m per 1000 kg, at full lock
    // How far from upright counts as overturned: the cosine of the angle
    // between the car's up and the world's. 0.35 is about 70 degrees, so a car
    // merely leaning on a kerb is left alone.
    float selfRightThreshold = 0.35f;
    float selfRightMaxSpin = 2.5f;    // rad/s cap, so it rocks rather than spins

    // ---- visuals -----------------------------------------------------------
    // Child nodes named <prefix>FL, FR, RL, RR are moved onto their wheels, then
    // steered and spun. Missing ones are simply not driven.
    std::string wheelNodePrefix = "Wheel";

    bool frontWheelDrive = false;
    bool rearWheelDrive = true;
    bool readsInput = true;

    SAIDA_REFLECT_BEHAVIOUR(VehicleBehaviour, "Vehicle")

    // ---- imperative (what an AI driver uses) --------------------------------
    void setThrottle(float value);   // -1..1; negative reverses
    void setBrake(float value);      // 0..1
    void setSteer(float value);      // -1 full left .. 1 full right
    void setHandbrake(bool engaged) { handbrake_ = engaged; }

    float throttle() const { return throttle_; }
    float steer() const { return steerInput_; }
    bool handbrake() const { return handbrake_; }

    // Signed speed along the vehicle's own forward, in m/s.
    float forwardSpeed() const { return forwardSpeed_; }
    float speed() const;
    int wheelsOnGround() const { return wheelsOnGround_; }
    bool isGrounded() const { return wheelsOnGround_ > 0; }

protected:
    // Driving input for this frame: x steers, y drives. Override to feed a
    // vehicle from a replay or a cutscene.
    virtual glm::vec2 readDriveInput();
    virtual bool readHandbrake();

private:
    struct Wheel {
        glm::vec3 anchor{0.0f};  // local
        bool steered = false;
        bool driven = false;
        bool braked = true;
        // Per-frame state, kept for the visuals and for the anti-roll bar.
        bool grounded = false;
        float compression = 0.0f;   // metres
        float load = 0.0f;          // N along the contact normal
        float suspensionLength = 0.0f;
        float spin = 0.0f;          // radians, accumulated for the visual
        class Node* visual = nullptr;
    };

    void layOutWheels();
    void updateVisuals();

    Wheel wheels_[4];
    float throttle_ = 0.0f;
    float brake_ = 0.0f;
    float steerInput_ = 0.0f;
    float steerAngle_ = 0.0f;   // degrees, smoothed
    bool handbrake_ = false;
    float forwardSpeed_ = 0.0f;
    int wheelsOnGround_ = 0;
    bool bindingsReady_ = false;
};

} // namespace saida
