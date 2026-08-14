#pragma once

#include "physics/CollisionObjectNode.hpp"

namespace saida {

// A body integrated by the solver (gravity, forces, collisions). Set `kinematic`
// for a body moved by code that still pushes dynamic bodies but isn't pushed back.
class RigidBodyNode : public CollisionObjectNode {
public:
    RigidBodyNode() : CollisionObjectNode("RigidBody") {}

    const char* typeName() const override { return "RigidBody"; }
    BodyMotion motion() const override {
        return kinematic ? BodyMotion::Kinematic : BodyMotion::Dynamic;
    }

    void serialize(nlohmann::json& j, ResourceManager& resources) const override;
    void deserialize(const nlohmann::json& j, ResourceManager& resources) override;

    bool kinematic = false;
    float mass = 1.0f;            // kg; <= 0 → derived from the shape
    float gravityFactor = 1.0f;   // 0 = floats, 1 = normal gravity
    float linearDamping = 0.05f;
    float angularDamping = 0.05f;
    // Where the mass sits, relative to where the collision shape puts it. Jolt
    // derives the centre of mass from the geometry, which is right for a crate
    // and wrong for anything whose mass is not spread through its own bounding
    // volume: a car keeps its engine, floorpan and occupants low while its box
    // has to cover the roof, and left at the box centre it tips at less than
    // its own tyres can pull. Zero keeps Jolt's answer.
    glm::vec3 centerOfMass{0.0f};

protected:
    void describeBody(BodyDesc& desc) const override {
        desc.mass = mass;
        desc.gravityFactor = gravityFactor;
        desc.linearDamping = linearDamping;
        desc.angularDamping = angularDamping;
        desc.centerOfMass = centerOfMass;
    }
};

} // namespace saida
