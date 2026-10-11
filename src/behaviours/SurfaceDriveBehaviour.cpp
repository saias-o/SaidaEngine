#include "behaviours/SurfaceDriveBehaviour.hpp"
#include "physics/RigidBodyNode.hpp"
#include <algorithm>
#include <cmath>

namespace saida {

void SurfaceDriveBehaviour::command(float throttle, float brake, float steer, bool handbrake) {
    throttle_ = std::isfinite(throttle) ? std::clamp(throttle, -1.f, 1.f) : 0.f;
    brake_ = std::isfinite(brake) ? std::clamp(brake, 0.f, 1.f) : 1.f;
    steer_ = std::isfinite(steer) ? std::clamp(steer, -1.f, 1.f) : 0.f;
    handbrake_ = handbrake;
}

void SurfaceDriveBehaviour::onPhysicsStep(float dt) {
    const float settings[]{acceleration,slopeAssist,braking,maxSpeed,reverseSpeed,wheelbase,steeringAngle,lateralAcceleration,
        yawAcceleration,groundReach,supportDistance,groundStability,groundDamping,selfRightAcceleration,selfRightSpeed,
        rollingDeceleration,aerodynamicDrag,gripResponse,steeringResponse};
    grounded_ = false;
    if(!std::isfinite(dt)||dt<=0.f||!std::isfinite(damage)||!std::isfinite(steeringBias))return;
    for(const auto value:settings)if(!std::isfinite(value)||value<0.f)return;
    auto* body = dynamic_cast<RigidBodyNode*>(node());
    if (!body || body->kinematic || !body->physicsWorld() || body->bodyId().IsInvalid()) return;
    auto& world = *body->physicsWorld();
    const auto id = body->bodyId();
    glm::vec3 position;
    glm::quat rotation;
    world.getBodyTransform(id, position, rotation);
    const glm::vec3 forward = rotation * glm::vec3(0, 0, -1);
    const glm::vec3 up = rotation * glm::vec3(0, 1, 0);
    const glm::vec3 velocity = world.linearVelocity(id);
    speed_ = glm::dot(velocity, forward);
    QueryFilter filter;
    filter.ignore = id;
    constexpr float kGroundNormalLimit = .5f;
    constexpr float kMinimumWheelbase = .1f;
    constexpr float kMinimumMass = 1.f;
    constexpr float kAxleSampleFraction = .4f;
    constexpr float kMaximumSupportSeparationSpeed = 1.f;
    const auto gravity = world.gravity() * body->gravityFactor;
    const glm::vec3 gravityUp = glm::dot(gravity,gravity) > 1e-6f ? -glm::normalize(gravity) : glm::vec3(0,1,0);
    if (!parked && selfRightAcceleration > 0.f && glm::dot(up,gravityUp) < .65f &&
        (std::abs(steer_) > .01f || std::abs(throttle_) > .01f)) {
        // Ground proximity gates recovery: direction input is not an airborne
        // flight control. The body still rotates through the contact solver.
        const auto road=world.raycast(position+gravityUp*.1f,-gravityUp,groundReach*2.f+.2f,filter);
        if(road.hit && glm::dot(road.normal,gravityUp)>.5f &&
           std::abs(glm::dot(velocity,gravityUp))<2.f) {
            auto axis=glm::cross(up,gravityUp);
            if(glm::dot(axis,axis)<.001f) {
                axis=forward-gravityUp*glm::dot(forward,gravityUp);
                if(glm::dot(axis,axis)<.001f)axis=rotation*glm::vec3(1,0,0);
                axis*=steer_<0.f?-1.f:1.f;
            }
            axis=glm::normalize(axis);
            const float spin=glm::dot(world.angularVelocity(id),axis);
            const float change=std::clamp(selfRightSpeed-spin,-selfRightAcceleration*dt,selfRightAcceleration*dt);
            world.applyAngularImpulse(id,axis*(world.effectiveAngularMass(id,axis)*change));
        }
    }
    glm::vec3 supportNormal(0.f);
    // Axle probes keep support over crests and ramp entries where the centre
    // is temporarily above empty space. These are queries, not suspension.
    for (float axle : {-1.f, 0.f, 1.f}) {
        const auto ground = world.raycast(position + forward * (axle * wheelbase * kAxleSampleFraction) + up * groundReach,
                                          -up, groundReach * 2.f, filter);
        // A nearby road is not necessarily carrying this body. In particular,
        // a pitched car must not count a tree wall as road and drive up it.
        if (ground.hit && ground.distance <= groundReach + supportDistance &&
            glm::dot(ground.normal, up) > kGroundNormalLimit &&
            glm::dot(ground.normal, gravityUp) > kGroundNormalLimit &&
            glm::dot(world.pointVelocity(id, ground.point), ground.normal) <= kMaximumSupportSeparationSpeed)
            supportNormal += ground.normal;
    }
    grounded_ = glm::dot(supportNormal,supportNormal) > 0.f;
    if (!grounded_) return;
    const float mass = std::max(kMinimumMass, body->mass);
    const float condition = 1.f - std::clamp(damage, 0.f, 1.f);
    const float requested = parked ? 0.f : std::clamp(steer_ + steeringBias, -1.f, 1.f);
    steering_ += (requested - steering_) * (1.f - std::exp(-std::max(0.f, steeringResponse) * dt));
    const glm::vec3 normal = glm::normalize(supportNormal);
    const glm::vec3 projectedForward = forward - normal * glm::dot(forward, normal);
    if (glm::dot(projectedForward,projectedForward) < .001f) return;
    const glm::vec3 heading = glm::normalize(projectedForward);
    speed_ = glm::dot(velocity, heading);
    const glm::vec3 lateral = glm::normalize(glm::cross(heading, normal));
    const float sideSpeed = glm::dot(velocity, lateral);
    const float grip = std::max(0.f, lateralAcceleration) * (handbrake_ ? .35f : .5f + .5f * condition);
    const float sideChange = std::clamp(-sideSpeed * (1.f - std::exp(-std::max(0.f, gripResponse) * dt)), -grip * dt, grip * dt);
    world.applyImpulse(id, lateral * (mass * sideChange));
    const float limit = throttle_ >= 0.f ? std::max(0.f, maxSpeed) : std::max(0.f, reverseSpeed);
    constexpr float kSpeedLimitBandFraction = .15f;
    const float direction = throttle_ >= 0.f ? 1.f : -1.f;
    // A limit in the requested direction must not suppress torque opposing rollback.
    const float speedFactor = std::clamp((limit - direction * speed_) /
        std::max(1.f, limit * kSpeedLimitBandFraction), 0.f, 1.f);
    const float gravityAlong = glm::dot(gravity, heading);
    // Optional arcade assistance offsets grade load only under power. Coasting,
    // downhill motion and airborne bodies retain the solver's gravity.
    const float gradeLoad = std::min(std::max(0.f, -direction * gravityAlong), lateralAcceleration);
    // Grade load is feed-forward torque, not acceleration headroom: tapering
    // it with the engine force would reintroduce uphill speed loss at cruise.
    const float drive = parked || condition <= 0.f ? 0.f : throttle_ *
        (acceleration * condition * speedFactor + std::clamp(slopeAssist, 0.f, 1.f) * gradeLoad);
    const float brake = (parked || handbrake_ ? 1.f : brake_) * std::max(0.f, braking);
    const float resistance = std::max(0.f, rollingDeceleration) + std::max(0.f, aerodynamicDrag) * speed_ * speed_;
    const float resistanceChange = std::copysign(std::min(std::abs(speed_), resistance * dt), speed_);
    const float afterDrive = speed_ + drive * dt - resistanceChange;
    // Include the upcoming solver gravity in the brake target. A stopped car
    // must hold a hill rather than gaining rollback every physics step.
    const float brakeChange = std::clamp(-afterDrive - gravityAlong * dt, -brake * dt, brake * dt);
    const float change = drive * dt - resistanceChange + brakeChange;
    world.applyImpulse(id, heading * (mass * change));
    const float base = std::max(kMinimumWheelbase, wheelbase);
    const float desiredYaw = -speed_ / base * std::tan(glm::radians(steeringAngle) * steering_);
    const float safeYaw = std::max(0.f, lateralAcceleration) / std::max(1.f, std::abs(speed_));
    const float currentYaw = glm::dot(world.angularVelocity(id), normal);
    const float delta = std::clamp(std::clamp(desiredYaw, -safeYaw, safeYaw) - currentYaw,
                                   -std::max(0.f, yawAcceleration) * dt, std::max(0.f, yawAcceleration) * dt);
    world.applyAngularImpulse(id, normal * (world.effectiveAngularMass(id, normal) * delta));

    if (groundStability > 0.f || groundDamping > 0.f) {
        // Resolve damping about the two road tangent directions separately. The
        // exact inertia and exponential decay prevent a narrow chassis from
        // receiving an angular impulse larger than its actual roll momentum.
        const float decay = 1.f - std::exp(-groundDamping * dt);
        const glm::vec3 restoring = glm::cross(up, normal) * (groundStability * dt);
        for (const auto axis : {heading, lateral}) {
            const float spin = glm::dot(world.angularVelocity(id), axis);
            const float correction = glm::dot(restoring, axis) - spin * decay;
            world.applyAngularImpulse(id, axis * (world.effectiveAngularMass(id, axis) * correction));
        }
    }
}

void SurfaceDriveBehaviour::describe(reflect::TypeBuilder<SurfaceDriveBehaviour>& t) {
    t.property("acceleration", &SurfaceDriveBehaviour::acceleration).range(0, 30);
    t.property("slopeAssist", &SurfaceDriveBehaviour::slopeAssist).range(0, 1);
    t.property("braking", &SurfaceDriveBehaviour::braking).range(0, 30);
    t.property("maxSpeed", &SurfaceDriveBehaviour::maxSpeed).range(0, 100);
    t.property("reverseSpeed", &SurfaceDriveBehaviour::reverseSpeed).range(0, 30);
    t.property("wheelbase", &SurfaceDriveBehaviour::wheelbase).range(.1, 20);
    t.property("steeringAngle", &SurfaceDriveBehaviour::steeringAngle).range(0, 60);
    t.property("lateralAcceleration", &SurfaceDriveBehaviour::lateralAcceleration).range(0, 20);
    t.property("yawAcceleration", &SurfaceDriveBehaviour::yawAcceleration).range(0, 20);
    t.property("groundReach", &SurfaceDriveBehaviour::groundReach).range(.1, 10);
    t.property("supportDistance", &SurfaceDriveBehaviour::supportDistance).range(0, 2);
    t.property("groundStability", &SurfaceDriveBehaviour::groundStability).range(0, 60);
    t.property("groundDamping", &SurfaceDriveBehaviour::groundDamping).range(0, 60);
    t.property("selfRightAcceleration", &SurfaceDriveBehaviour::selfRightAcceleration).range(0, 30);
    t.property("selfRightSpeed", &SurfaceDriveBehaviour::selfRightSpeed).range(0, 5);
    t.property("rollingDeceleration", &SurfaceDriveBehaviour::rollingDeceleration).range(0, 5);
    t.property("aerodynamicDrag", &SurfaceDriveBehaviour::aerodynamicDrag).range(0, .1);
    t.property("gripResponse", &SurfaceDriveBehaviour::gripResponse).range(0, 60);
    t.property("steeringResponse", &SurfaceDriveBehaviour::steeringResponse).range(0, 60);
    t.property("damage", &SurfaceDriveBehaviour::damage).range(0, 1);
    t.property("steeringBias", &SurfaceDriveBehaviour::steeringBias).range(-1, 1);
    t.property("parked", &SurfaceDriveBehaviour::parked);
}

} // namespace saida
