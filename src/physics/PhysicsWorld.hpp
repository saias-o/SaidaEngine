#pragma once

// Jolt config header first.
#include <Jolt/Jolt.h>
#include <Jolt/Physics/Body/BodyID.h>
#include <Jolt/Core/Reference.h>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <memory>
#include <vector>

namespace JPH {
class PhysicsSystem;
class BodyInterface;
class Shape;
class TempAllocator;
class JobSystem;
class CharacterVirtual;
class TwoBodyConstraint;
} // namespace JPH

namespace saida {

class TriggerContactListener;

// How a body is integrated by the simulation.
enum class BodyMotion { Static, Kinematic, Dynamic };

struct RaycastHit {
    bool hit = false;
    glm::vec3 point{0.0f};
    glm::vec3 normal{0.0f};
    float distance = 0.0f;
    JPH::BodyID body;  // invalid when !hit
};

// Filters shared by the scene queries (raycast / overlap). Sensors (Area
// triggers) are excluded by default: a camera occlusion ray or a hitscan must
// not stop on an invisible trigger volume.
struct QueryFilter {
    JPH::BodyID ignore;       // body to skip (typically the caster's own body)
    bool hitSensors = false;  // true → sensor bodies are reported too
};

// Everything needed to spawn a body. The shape must already be built (and is
// ref-counted by the resulting body).
struct BodyDesc {
    const JPH::Shape* shape = nullptr;
    glm::vec3 position{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    BodyMotion motion = BodyMotion::Dynamic;
    bool isSensor = false;
    float friction = 0.5f;
    float restitution = 0.0f;
    // Dynamic-only:
    float mass = 1.0f;             // <= 0 → use the shape's computed mass
    float gravityFactor = 1.0f;
    float linearDamping = 0.05f;
    float angularDamping = 0.05f;
    void* userData = nullptr;       // stored on the body (we map it to the owning node)
};

// One physics world per scene. Wraps a Jolt PhysicsSystem with a fixed-timestep
// accumulator. Jolt's global state (allocator/factory/types) is reference-counted
// across worlds, so constructing/destroying multiple is safe.
class PhysicsWorld {
public:
    PhysicsWorld();
    ~PhysicsWorld();
    PhysicsWorld(const PhysicsWorld&) = delete;
    PhysicsWorld& operator=(const PhysicsWorld&) = delete;

    // Advance the simulation by `dt`, sub-stepped at a fixed 1/60 s.
    void step(float dt);

    // Create a body from an already-built (ref-counted) shape. Returns an
    // invalid id on failure.
    JPH::BodyID createBody(const BodyDesc& desc);
    void removeBody(JPH::BodyID id);

    // Teleport a body (used for static/kinematic bodies driven by the node tree).
    void setBodyTransform(JPH::BodyID id, const glm::vec3& position,
                          const glm::quat& rotation, bool activate);
    // Smoothly move a kinematic body toward a target over `dt` (proper kinematic contacts).
    void moveKinematic(JPH::BodyID id, const glm::vec3& position,
                       const glm::quat& rotation, float dt);
    // Read a body's world transform back (used to drive dynamic nodes).
    void getBodyTransform(JPH::BodyID id, glm::vec3& position, glm::quat& rotation) const;

    // Set a body's velocity (and activate it). Used e.g. to throw a released
    // grabbed object with the hand's motion.
    void setLinearVelocity(JPH::BodyID id, const glm::vec3& velocity);
    void setAngularVelocity(JPH::BodyID id, const glm::vec3& velocity);

    RaycastHit raycast(const glm::vec3& origin, const glm::vec3& direction,
                       float maxDistance, const QueryFilter& filter = {}) const;

    // All bodies whose shape intersects the sphere. Order is unspecified;
    // each body is reported once.
    std::vector<JPH::BodyID> overlapSphere(const glm::vec3& center, float radius,
                                           const QueryFilter& filter = {}) const;

    // Register a built Jolt constraint joining `a` and `b` (either may be
    // invalid for a world-anchored constraint). The world owns a reference and
    // removes the constraint automatically when one of its bodies is removed.
    void addConstraint(JPH::Ref<JPH::TwoBodyConstraint> constraint);
    void removeConstraint(const JPH::Ref<JPH::TwoBodyConstraint>& constraint);

    // Create a kinematic character controller (cf. Godot CharacterBody3D). NOT a
    // simulated body: the caller owns the ref and moves it via updateCharacter.
    JPH::Ref<JPH::CharacterVirtual> createCharacter(const JPH::Shape* shape,
                                                    const glm::vec3& position,
                                                    const glm::quat& rotation,
                                                    float mass, float maxSlopeAngleRad,
                                                    void* userData);
    // Move a character through the world for `dt`: slide along geometry, walk up
    // stairs, stick to the floor, push dynamic bodies. Set its linear velocity first.
    void updateCharacter(JPH::CharacterVirtual& character, float dt);

    // A contact between two bodies began (entered) or ended. `sensor` is true when
    // a trigger (Area) was involved — those go to AreaNode overlap, the rest to the
    // bodies' collision signals.
    struct ContactEvent {
        JPH::BodyID a;
        JPH::BodyID b;
        bool entered;
        bool sensor;
    };
    // Collect (and clear) the sensor overlap events accumulated during the last step.
    std::vector<ContactEvent> drainContactEvents();
    // The CollisionObjectNode pointer stored on a body (null if none / invalid id).
    void* bodyUserData(JPH::BodyID id) const;

    JPH::PhysicsSystem& system() { return *system_; }

private:
    struct LayerState;  // holds the three layer-filter interface impls
    std::unique_ptr<LayerState> layers_;
    std::unique_ptr<JPH::TempAllocator> tempAllocator_;
    std::unique_ptr<JPH::JobSystem> jobSystem_;
    std::unique_ptr<JPH::PhysicsSystem> system_;
    std::unique_ptr<TriggerContactListener> contactListener_;
    // Live constraints, so removeBody can drop any constraint still attached to
    // a body before Jolt destroys it (a dangling constraint would crash the step).
    std::vector<JPH::Ref<JPH::TwoBodyConstraint>> constraints_;

    float accumulator_ = 0.0f;
};

} // namespace saida
