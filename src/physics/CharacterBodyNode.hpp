#pragma once

#include "physics/CollisionObjectNode.hpp"

#include <Jolt/Core/Reference.h>

#include <glm/glm.hpp>

#include <vector>

namespace JPH {
class CharacterVirtual;
}

namespace saida {

// Jolt CharacterVirtual controller. Gameplay writes velocity; the engine moves it
// during the physics step and updates floor state afterward.
class CharacterBodyNode : public CollisionObjectNode {
public:
    CharacterBodyNode();   // out-of-line (Ref<CharacterVirtual> member needs the complete type)
    ~CharacterBodyNode() override;

    const char* typeName() const override { return "CharacterBody"; }
    BodyMotion motion() const override { return BodyMotion::Kinematic; }
    CharacterBodyNode* asCharacterBody() override { return this; }
    JPH::BodyID innerBodyId() const override;  // the CharacterVirtual's inner body

    void serialize(nlohmann::json& j, ResourceManager& resources) const override;
    void deserialize(const nlohmann::json& j, ResourceManager& resources) override;

    // Engine hooks (not called by gameplay code).
    void syncToPhysics(PhysicsWorld& world) override;     // lazily create the character
    void prePhysicsStep(PhysicsWorld& world, float dt) override;  // move/slide
    bool hasPrePhysicsStep() const override { return true; }
    void syncFromPhysics(PhysicsWorld& world) override;   // write pose back to the node
    void rebasePhysics(const glm::vec3& translation, const glm::quat& rotation) override;

    glm::vec3 velocity{0.0f};     // read/written by the controller behaviour
    float maxSlopeAngle = 50.0f;  // degrees; steeper ground → isOnSteepSlope(), slide off
    float mass = 70.0f;           // how hard the character pushes dynamic bodies

    bool isOnFloor() const;
    bool isOnSteepSlope() const;
    glm::vec3 groundNormal() const;
    glm::vec3 groundVelocity() const;  // velocity of the floor (moving platforms)
    // Move-and-slide now rather than in the next physics step, for gameplay
    // that needs the answer this frame or owns its character's position (in
    // its own units, on a globe): from where the node stands, at `velocity`
    // for `dt`, stopping at every body and sliding along it. The node is put
    // where the character ended, the next step leaves it there, and the
    // position is returned (world space); `velocity` is left as it was. Until
    // its first physics sync the character has no body yet and moves unhindered.
    glm::vec3 moveAndSlide(const glm::vec3& velocity, float dt);

    // The bodies touched by the last move (PhysicsWorld::characterContacts),
    // each named by its node.
    struct Contact {
        CollisionObjectNode* node = nullptr;
        glm::vec3 point{0.0f};
        glm::vec3 normal{0.0f};  // points back at the character
    };
    std::vector<Contact> contacts() const;

private:
    JPH::Ref<JPH::CharacterVirtual> character_;
    bool moved_ = false;  // by moveAndSlide since the last step
};

} // namespace saida
