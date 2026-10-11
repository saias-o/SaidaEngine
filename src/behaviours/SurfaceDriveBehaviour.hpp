#pragma once

#include "core/Reflection.hpp"
#include "scene/Behaviour.hpp"

namespace saida {

// Force-based ground vehicle for inexpensive AI, keyboard, stick or replay input.
// Local -Z is forward. The collider carries the body; no wheel springs are required.
class SurfaceDriveBehaviour : public Behaviour {
public:
    float acceleration = 7.f;
    float slopeAssist = 0.f; // Powered uphill gravity compensation, 0 = physical grade load.
    float braking = 10.f;
    float maxSpeed = 28.f;
    float reverseSpeed = 9.f;
    float wheelbase = 2.5f;
    float steeringAngle = 32.f;
    float lateralAcceleration = 8.f;
    float yawAcceleration = 3.f;
    float groundReach = 1.2f;
    float supportDistance = .15f; // Maximum road gap below the body origin at an axle.
    float groundStability = 0.f; // Optional grounded tilt correction, rad/s^2 per radian.
    float groundDamping = 0.f; // Optional grounded tilt damping, per second.
    float selfRightAcceleration = 0.f; // Direction input progressively rights a stranded body.
    float selfRightSpeed = 2.f;
    float rollingDeceleration = .25f;
    float aerodynamicDrag = .0005f;
    float gripResponse = 10.f;
    float steeringResponse = 6.f;
    float damage = 0.f;
    float steeringBias = 0.f;
    bool parked = true;

    void command(float throttle, float brake, float steer, bool handbrake = false);
    void onPhysicsStep(float dt) override;
    bool hasPhysicsStep() const override { return true; }
    float signedSpeed() const { return speed_; }
    float steering() const { return steering_; }
    bool grounded() const { return grounded_; }
    SAIDA_REFLECT_BEHAVIOUR(SurfaceDriveBehaviour, "SurfaceDrive")
private:
    float throttle_ = 0.f, brake_ = 0.f, steer_ = 0.f, steering_ = 0.f, speed_ = 0.f;
    bool handbrake_ = false, grounded_ = false;
};

} // namespace saida
