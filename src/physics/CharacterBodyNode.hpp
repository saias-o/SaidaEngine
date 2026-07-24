#pragma once

#include "physics/CollisionObjectNode.hpp"

#include <Jolt/Core/Reference.h>

#include <glm/glm.hpp>

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

    void serialize(nlohmann::json& j, ResourceManager& resources) const override;
    void deserialize(const nlohmann::json& j, ResourceManager& resources) override;

    // Engine hooks (not called by gameplay code).
    void syncToPhysics(PhysicsWorld& world) override;     // lazily create the character
    void prePhysicsStep(PhysicsWorld& world, float dt) override;  // move/slide
    void syncFromPhysics(PhysicsWorld& world) override;   // write pose back to the node

    glm::vec3 velocity{0.0f};     // read/written by the controller behaviour
    float maxSlopeAngle = 50.0f;  // degrees; steeper ground → isOnSteepSlope(), slide off
    float mass = 70.0f;           // how hard the character pushes dynamic bodies

    bool isOnFloor() const;
    bool isOnSteepSlope() const;
    glm::vec3 groundNormal() const;
    glm::vec3 groundVelocity() const;  // velocity of the floor (moving platforms)

private:
    JPH::Ref<JPH::CharacterVirtual> character_;
};

} // namespace saida
