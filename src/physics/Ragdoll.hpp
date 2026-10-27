#pragma once

// A skeleton handed to the physics: one capsule body per chosen bone, jointed
// to its parent part with anatomical limits, posed from an animation and read
// back as one. What a character becomes when it is knocked down, shot or run
// over -- the fall is the solver's, not a clip's.
//
// The ragdoll is built from a Rig and a RagdollProfile (which bones, how
// thick, how heavy, how far each joint turns) and lives in a PhysicsWorld only
// between spawn() and despawn(). Its bodies are ordinary dynamic Jolt bodies:
// they collide with the world, with vehicles and with other ragdolls, are hit
// by raycasts, and sleep when they come to rest. Their user data is null,
// which is what keeps the scene from mistaking them for a node's body; a game
// asks partOf() which ragdoll a hit body belongs to.
//
// Joint limits are centred on the rig's bind pose, not on the pose it spawns
// from, so a body thrown mid-stride still bends the way a body bends. Knees
// and elbows are hinges; everything else is a swing-twist cone.

#include "physics/PhysicsWorld.hpp"
#include "scene/animation/Pose.hpp"
#include "scene/animation/Rig.hpp"

#include <Jolt/Core/Reference.h>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace JPH {
class GroupFilterTable;
} // namespace JPH

namespace saida {

enum class RagdollJoint { Cone, Hinge };

struct RagdollPart {
    std::string bone;   // the bone this body drives
    std::string end;    // the bone its capsule reaches (usually a child or grandchild)
    // Optional earlier part's bone for rigs with disconnected skin roots.
    // Empty selects the nearest skeletal ancestor, as usual. A disconnected
    // driven root also receives position from physics when reading the pose.
    std::string parent;
    // For a bone with no useful end (a head), the capsule runs `length` along
    // `axis` (both in the bone's bind-pose object frame) from the bone.
    float length = 0.0f;
    glm::vec3 axis{0.0f, 1.0f, 0.0f};
    float radius = 0.08f;     // rig (object) units
    float massShare = 0.1f;   // relative; the shares are normalised over the profile
    RagdollJoint joint = RagdollJoint::Cone;
    float swing = 0.7f;       // Cone: half angle of the swing cone, radians
    float twist = 0.35f;      // Cone: twist either way about the bone, radians
    // Cone: the cone's centre leans this far (radians) from the bind pose
    // toward `bendToward`, so a hip folds forward further than it extends.
    float swingTilt = 0.0f;
    // Hinge: the limb bends toward `bendToward` (object space, bind pose)
    // between `bendMin` and `bendMax` radians from the bind pose. Cone: the
    // way its centre leans (swingTilt).
    glm::vec3 bendToward{0.0f, 0.0f, 1.0f};
    float bendMin = -0.05f, bendMax = 2.4f;
};

struct RagdollProfile {
    // Parents before children. The first part is the root: it alone gives its
    // bone a position; the others only turn theirs. Each later part joins the
    // nearest earlier part among its bone's ancestors, unless parent names an
    // earlier part explicitly (disconnected skin roots).
    std::vector<RagdollPart> parts;
    float mass = 70.0f;  // kg, the whole body
    float friction = 0.8f, restitution = 0.05f;
    float linearDamping = 0.05f, angularDamping = 0.3f;
    // Friction torque (N.m) in every joint: a little muscle tone, so a body
    // folds rather than flaps. Zero is a limp chain.
    float jointFriction = 1.0f;
    // A ragdoll whose fastest part moves slower than this (m/s) for
    // `settleTime` seconds, or whose bodies are all asleep, has settled.
    float settleSpeed = 0.25f;
    float settleTime = 0.6f;
};

// The usual humanoid: pelvis, chest, head, upper and lower arms, thighs and
// shins (eleven bodies). Names are the rig's; `forward` is the way the
// character faces in object space. Shares and limits are game-plausible
// values for an adult, not anthropometric measurements.
struct HumanoidBones {
    std::string pelvis, spine, chest, neck, head;
    std::string leftUpperArm, leftForearm, leftHand, rightUpperArm, rightForearm, rightHand;
    std::string leftThigh, leftShin, leftFoot, rightThigh, rightShin, rightFoot;
};
RagdollProfile humanoidRagdoll(const HumanoidBones& bones, float mass = 70.0f,
                               const glm::vec3& forward = {0.0f, 0.0f, 1.0f}, float limbRadius = 0.06f);

class Ragdoll {
public:
    Ragdoll(const Rig& rig, RagdollProfile profile);
    ~Ragdoll();
    Ragdoll(const Ragdoll&) = delete;
    Ragdoll& operator=(const Ragdoll&) = delete;

    // Every named bone exists and every part has a length; otherwise error()
    // says which, and spawn() refuses.
    bool valid() const { return error_.empty(); }
    const std::string& error() const { return error_; }
    const RagdollProfile& profile() const { return profile_; }
    size_t partCount() const { return parts_.size(); }

    // Builds the bodies at the pose `pose` (object space, as Animator::
    // globalPose) placed by `objectToWorld` (rotation, translation, uniform
    // scale), moving at `velocity`. `local` is the same pose's local form:
    // bones no part drives keep it while the ragdoll is up. Returns false --
    // creating nothing -- when the ragdoll is invalid, already spawned, or the
    // world refused a body (PhysicsWorld logs why).
    bool spawn(PhysicsWorld& world, const GlobalPose& pose, const LocalPose& local,
               const glm::mat4& objectToWorld, const glm::vec3& velocity = glm::vec3(0.0f),
               const glm::vec3& angularVelocity = glm::vec3(0.0f));
    void despawn();
    bool spawned() const { return world_ != nullptr; }

    // The bodies' pose as the skeleton's local pose, seen from a node now at
    // `objectToWorld`.
    void readPose(const glm::mat4& objectToWorld, LocalPose& out) const;

    // An impulse (N.s) at a world point, on the part nearest to it.
    void applyImpulse(const glm::vec3& impulse, const glm::vec3& worldPoint);
    void applyImpulse(size_t part, const glm::vec3& impulse, const glm::vec3& worldPoint);
    // -1 when `body` is none of this ragdoll's.
    int partOf(JPH::BodyID body) const;
    size_t nearestPart(const glm::vec3& worldPoint) const;
    JPH::BodyID body(size_t part) const;
    // World position of the root part's bone (the pelvis), and of a part's body.
    glm::vec3 rootPosition() const;
    glm::quat rootRotation() const;
    glm::vec3 partPosition(size_t part) const;
    // World position of the bone a part drives (its joint with its parent).
    glm::vec3 bonePosition(size_t part) const;
    // Fastest linear speed among the parts, m/s.
    float fastestSpeed() const;

    // Advances the settling clock; true once settled (see RagdollProfile).
    bool updateSettled(float dt);
    bool settled() const { return settled_; }

    // Old world point p is now rotation*p + translation (Scene::rebaseSubtree).
    void rebase(const glm::vec3& translation, const glm::quat& rotation);

private:
    struct Part {
        int32_t bone = -1, end = -1, parent = -1;  // parent: index into parts_
        bool drivesPosition = false;
        float length = 0.0f, radius = 0.0f, mass = 0.0f;
        glm::vec3 restDirection{0.0f, 1.0f, 0.0f};  // bone-local, toward the capsule's end
        JPH::BodyID body;
        glm::quat boneInBodyRotation{1.0f, 0.0f, 0.0f, 0.0f};
        glm::vec3 boneInBodyPosition{0.0f};  // world units
    };
    void build(const Rig& rig);

    const Rig& rig_;
    RagdollProfile profile_;
    std::string error_;
    std::vector<Part> parts_;
    std::vector<int32_t> partOfBone_;  // per bone: driving part or -1
    std::vector<glm::mat4> restGlobal_;
    LocalPose captured_;
    mutable std::vector<glm::mat4> scratch_;
    JPH::Ref<JPH::GroupFilterTable> filter_;
    uint32_t group_ = 0;
    PhysicsWorld* world_ = nullptr;
    float worldScale_ = 1.0f;
    float calm_ = 0.0f;
    bool settled_ = false;
};

} // namespace saida
